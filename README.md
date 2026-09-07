# tpgestd

Minimal 3/4-finger touchpad gesture daemon for Xorg, built directly on
`libinput`.

## Why this is lightweight / battery-friendly

- **No polling loop.** The only blocking call is `poll(fd, -1)` on
  libinput's own fd (which wraps an internal epoll over the touchpad's
  evdev node). The kernel does not wake this process on a timer — only
  when your fingers actually move on the pad. Idle CPU usage is 0%.
- **No X11 connection, no Xlib, no toolkit.** It doesn't talk to the X
  server at all for *reading* gestures — it reads raw evdev via
  libinput, which is what Xorg's own synaptics/libinput driver already
  does. It only shells out (`xdotool`/`wmctrl`/whatever you configure)
  the moment a real gesture completes.
- **~350 lines, one source file**, dynamically linked only against
  `libinput` and `libudev` (both already on your system if you have a
  Wayland-or-Xorg touchpad working at all).
- Gesture recognition (finger count, swipe direction, pinch scale)
  is done by libinput itself — no re-implementing evdev multitouch
  parsing.

## Build

```sh
gcc -O2 -o tpgestd tpgestd.c $(pkg-config --cflags --libs libinput libudev)
```

Requires `libinput-dev`/`libinput-devel` and `libudev-dev` at build
time only (runtime just needs the shared libs, which you already have).

## Permissions

libinput needs to open the raw `/dev/input/event*` node for your
touchpad. On a normal systemd-logind seat0 session this works out of
the box for your own user (logind grants device ACLs to the active
session). If you get a permissions error on `seat0` assignment:

```sh
sudo usermod -aG input $USER
# log out and back in
```

## Configure

Copy `config.example` to `~/.config/tpgestd/config` and edit. Format:

```
swipe 3 left  = wmctrl -s -1
swipe 4 up    = xdotool getactivewindow windowmaximize
pinch 3 in    = xdotool key ctrl+minus
```

Fingers: `3` or `4`. Swipe directions: `up down left right`. Pinch
directions: `in out`. Commands run async via `sh -c`, so the gesture
loop is never blocked waiting on them.

`xdotool` and `wmctrl` are the usual glue for Xorg window/workspace
actions — install whichever you need for your commands
(`xdotool`, `wmctrl`, or your WM's own CLI, e.g. AwesomeWM's `awesome-client`).

## Run

```sh
./tpgestd                      # uses ~/.config/tpgestd/config
./tpgestd -c ./my-config       # explicit config path
```

## Autostart (systemd user service)

```sh
mkdir -p ~/.local/bin ~/.config/systemd/user
cp tpgestd ~/.local/bin/
cp tpgestd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now tpgestd.service
```

The unit also sets `Nice=10`, `IOSchedulingClass=idle`, and a low
`CPUWeight` — belt-and-suspenders, since the process is parked in
`poll()` almost all the time regardless.

## Tuning thresholds

Two constants near the top of `tpgestd.c` control gesture sensitivity:

- `swipe_threshold` (default `40.0`, in accumulated pixels) — how far
  you must move before a swipe direction is recognized.
- `pinch_threshold` (default `0.15`, fractional scale change) — how
  much pinch/spread before it's recognized as in/out.

Raise either if gestures fire too easily; lower if they feel sluggish.
