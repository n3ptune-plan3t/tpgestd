/*
 * tpgestd — minimal 3/4-finger touchpad gesture daemon for Xorg
 *
 * Design goals:
 *   - No polling loop. Blocks in poll(2) on libinput's single fd, which
 *     is only made readable by the kernel when a real input event
 *     arrives (epoll under the hood). The process is asleep 100% of
 *     idle time — nothing wakes the CPU on a timer.
 *   - Single source file, no daemon framework, no dependencies beyond
 *     libinput + libudev (both already present on any Xorg system with
 *     a touchpad).
 *   - Recognizes 3- and 4-finger swipes (up/down/left/right) and
 *     3-/4-finger pinch (in/out), and runs a user-defined shell
 *     command per gesture, read from a plain-text config file.
 *
 * Build:  gcc -O2 -o tpgestd tpgestd.c $(pkg-config --cflags --libs libinput libudev)
 * Run:    tpgestd [-c /path/to/config]
 *
 * Permissions: reads raw evdev nodes via libinput. On a normal seat0
 * logind session this just works as your own user. If it doesn't,
 * add yourself to the `input` group:  sudo usermod -aG input $USER
 * (log out/in afterwards).
 */

#define _GNU_SOURCE
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

/* ---------- config ---------- */

typedef enum { GT_SWIPE, GT_PINCH } gesture_type_t;

typedef struct action {
    gesture_type_t type;
    int fingers;            /* 3 or 4 */
    /* for swipe: 0=up 1=down 2=left 3=right ; for pinch: 0=in 1=out */
    int direction;
    char *command;
    struct action *next;
} action_t;

static action_t *actions = NULL;

/* Minimum accumulated motion (px) / scale delta before a swipe/pinch
 * counts as a directional gesture, to filter out noise/jitter. */
static double swipe_threshold = 40.0;
static double pinch_threshold = 0.15;

static int dir_from_name(const char *s) {
    if (!strcmp(s, "up")) return 0;
    if (!strcmp(s, "down")) return 1;
    if (!strcmp(s, "left")) return 2;
    if (!strcmp(s, "right")) return 3;
    if (!strcmp(s, "in")) return 0;
    if (!strcmp(s, "out")) return 1;
    return -1;
}

/* config line syntax:
 *   swipe 3 left  = wmctrl -s -1
 *   swipe 4 up    = xdotool key super
 *   pinch 3 in    = xdotool key ctrl+minus
 *   # comments and blank lines ignored
 */
static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "tpgestd: could not open config '%s': %s\n",
                path, strerror(errno));
        exit(1);
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char kind[16], fingers_s[8], dir_s[16];
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *cmd = eq + 1;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        size_t clen = strlen(cmd);
        while (clen > 0 && (cmd[clen-1] == '\n' || cmd[clen-1] == '\r' ||
                             cmd[clen-1] == ' ')) cmd[--clen] = '\0';
        if (clen == 0) continue;

        if (sscanf(p, "%15s %7s %15s", kind, fingers_s, dir_s) != 3) {
            fprintf(stderr, "tpgestd: config line %d malformed, skipping\n", lineno);
            continue;
        }

        action_t *a = calloc(1, sizeof(*a));
        a->fingers = atoi(fingers_s);
        a->direction = dir_from_name(dir_s);
        a->command = strdup(cmd);

        if (!strcmp(kind, "swipe")) a->type = GT_SWIPE;
        else if (!strcmp(kind, "pinch")) a->type = GT_PINCH;
        else {
            fprintf(stderr, "tpgestd: config line %d unknown gesture type '%s'\n",
                    lineno, kind);
            free(a->command);
            free(a);
            continue;
        }

        if (a->direction < 0 || (a->fingers != 3 && a->fingers != 4)) {
            fprintf(stderr, "tpgestd: config line %d invalid fingers/direction\n", lineno);
            free(a->command);
            free(a);
            continue;
        }

        a->next = actions;
        actions = a;
    }
    fclose(f);
}

static void run_command(const char *cmd) {
    /* Fire-and-forget: fork, exec via sh -c, reap asynchronously via
     * SIGCHLD ignore (SA_NOCLDWAIT) so we never block the event loop. */
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* parent: don't wait, SIGCHLD is set to be auto-reaped (see main) */
}

static void dispatch_action(gesture_type_t type, int fingers, int direction) {
    for (action_t *a = actions; a; a = a->next) {
        if (a->type == type && a->fingers == fingers && a->direction == direction) {
            run_command(a->command);
            return;
        }
    }
}

/* ---------- libinput open/close via plain fds (no seat mgmt needed for a single user session) ---------- */

static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

/* ---------- gesture state ---------- */

static double acc_dx = 0, acc_dy = 0, acc_scale_start = 1.0;
static int active_fingers = 0;

static void handle_swipe(struct libinput_event *ev, enum libinput_event_type t) {
    struct libinput_event_gesture *g = libinput_event_get_gesture_event(ev);
    if (t == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
        acc_dx = acc_dy = 0;
        active_fingers = libinput_event_gesture_get_finger_count(g);
    } else if (t == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
        acc_dx += libinput_event_gesture_get_dx(g);
        acc_dy += libinput_event_gesture_get_dy(g);
    } else if (t == LIBINPUT_EVENT_GESTURE_SWIPE_END) {
        if (active_fingers == 3 || active_fingers == 4) {
            double adx = fabs(acc_dx), ady = fabs(acc_dy);
            if (adx > swipe_threshold || ady > swipe_threshold) {
                int dir;
                if (adx > ady)
                    dir = acc_dx > 0 ? 3 : 2;   /* right : left */
                else
                    dir = acc_dy > 0 ? 1 : 0;   /* down : up */
                dispatch_action(GT_SWIPE, active_fingers, dir);
            }
        }
        active_fingers = 0;
    }
}

static void handle_pinch(struct libinput_event *ev, enum libinput_event_type t) {
    struct libinput_event_gesture *g = libinput_event_get_gesture_event(ev);
    if (t == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN) {
        acc_scale_start = 1.0;
        active_fingers = libinput_event_gesture_get_finger_count(g);
    } else if (t == LIBINPUT_EVENT_GESTURE_PINCH_UPDATE) {
        acc_scale_start = libinput_event_gesture_get_scale(g);
    } else if (t == LIBINPUT_EVENT_GESTURE_PINCH_END) {
        if (active_fingers == 3 || active_fingers == 4) {
            double delta = acc_scale_start - 1.0;
            if (fabs(delta) > pinch_threshold)
                dispatch_action(GT_PINCH, active_fingers, delta > 0 ? 1 : 0);
        }
        active_fingers = 0;
    }
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    char default_path[PATH_MAX];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [-c config_file]\n", argv[0]);
            return 0;
        }
    }
    if (!config_path) {
        const char *home = getenv("HOME");
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg)
            snprintf(default_path, sizeof default_path, "%s/tpgestd/config", xdg);
        else
            snprintf(default_path, sizeof default_path, "%s/.config/tpgestd/config", home);
        config_path = default_path;
    }
    load_config(config_path);
    if (!actions) {
        fprintf(stderr, "tpgestd: no valid actions loaded from '%s', exiting\n", config_path);
        return 1;
    }

    /* Reap children automatically without blocking on wait(). */
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, NULL);

    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "tpgestd: udev_new failed\n");
        return 1;
    }

    struct libinput *li = libinput_udev_create_context(&interface, NULL, udev);
    if (!li) {
        fprintf(stderr, "tpgestd: libinput_udev_create_context failed\n");
        return 1;
    }
    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "tpgestd: failed to assign seat0 (permissions? try adding "
                        "yourself to the 'input' group)\n");
        return 1;
    }

    int fd = libinput_get_fd(li);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    for (;;) {
        /* Blocks here — no timeout, no busy loop. The kernel only wakes
         * this thread when libinput's fd actually has data. */
        int r = poll(&pfd, 1, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        libinput_dispatch(li);
        struct libinput_event *ev;
        while ((ev = libinput_get_event(li)) != NULL) {
            enum libinput_event_type t = libinput_event_get_type(ev);
            switch (t) {
                case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
                case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
                case LIBINPUT_EVENT_GESTURE_SWIPE_END:
                    handle_swipe(ev, t);
                    break;
                case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
                case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
                case LIBINPUT_EVENT_GESTURE_PINCH_END:
                    handle_pinch(ev, t);
                    break;
                default:
                    break;
            }
            libinput_event_destroy(ev);
        }
    }

    libinput_unref(li);
    udev_unref(udev);
    return 0;
}
