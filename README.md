# benchpi

`benchpi`, a touchscreen status panel for a Raspberry Pi 5 with a 480x320
ILI9486 SPI LCD hat, running on the bare Linux framebuffer. No X11, no
Wayland, no compositor, no browser.

```
+--------------------------------------------------+
| benchpi                          eth0  192.0.2.17 |
| +------------+ +------------+ +----------------+  |
| |   49.0C    | |    13%     | |     0.03       |  |
| |  CPU TEMP  | |   MEMORY   | |     LOAD       |  |
| +------------+ +------------+ +----------------+  |
| +----------------------------------------------+  |
| | USB                                  up 22m  |  |
| | CH341A programmer                            |  |
| | FT232R USB UART                              |  |
| +----------------------------------------------+  |
| [    REBOOT    ]          [    SHUTDOWN    ]      |
+--------------------------------------------------+
```

## Hardware

| Part      | Detail                                                    |
|-----------|-----------------------------------------------------------|
| Board     | Raspberry Pi 5, 2GB, Debian 13 (trixie), Linux 6.18        |
| Display   | `/dev/fb0`, `fb_ili9486`, 480x320, RGB565, 960 byte stride |
| Touch     | `/dev/input/event1`, ADS7846/XPT2046, `spi0.1`             |

Touch orientation is corrected in the device tree with `swapxy,invy`, so the
evdev coordinates already line up with the landscape framebuffer.

The user running it needs the `video` and `input` groups. The app itself
never needs root.

## Building

LVGL is not packaged in Debian, so the Makefile fetches the release tarball.
It is not checked in.

```sh
make lvgl        # download LVGL 9.5.0 into ./lvgl
make -j4         # ~700 translation units, under 30 seconds on a Pi 5
```

`make lvgl` has to run first: the source list is a `find` expanded when the
Makefile is parsed.

## Running

Take the kernel console off the framebuffer first, or it will corrupt the UI.
See "The console owns fb0 too" below for why.

```sh
sudo systemctl disable --now gpm      # reads the touchscreen as a mouse
echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind
./benchpi
```

`echo 1` to that same file gives the console back.

The REBOOT and SHUTDOWN buttons additionally need a sudoers drop-in, or they
will display `NO SUDO` and do nothing:

```sh
sed "s/YOURUSER/$USER/" benchpi.sudoers \
    | sudo install -o root -g root -m 0440 /dev/stdin /etc/sudoers.d/benchpi
```

The `sed` fills in whoever runs the dashboard. It grants exactly
`systemctl poweroff` and `systemctl reboot`, and nothing else. It is needed
because polkit treats the dashboard's session as remote, having no seat, and
demands authentication for `org.freedesktop.login1.power-off`, which a
touchscreen cannot supply. `pkcheck` says so directly:

```
$ pkcheck --action-id org.freedesktop.login1.power-off --process $$
Authorization requires authentication and -u wasn't passed.
```

Running the whole UI as root to avoid that one file would be a much worse
trade.

## Starting it at boot

```sh
sed "s/YOURUSER/$USER/g; s|YOURHOME|$HOME|" benchpi.service \
    | sudo tee /etc/systemd/system/benchpi.service
sudo systemctl enable --now benchpi
```

The unit runs the UI unprivileged and unbinds the console in an
`ExecStartPre=+` line, which systemd runs as root regardless of `User=`. The
manual `echo 0 > .../vtcon1/bind` does not survive a reboot, so without this
the LCD comes back up showing a login prompt.

Stopping the service rebinds the console, so the panel becomes a terminal
again whenever the dashboard is not running.

## Using the CH341A without root

```sh
sudo cp 99-ch341a.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

flashrom's `ch341a_spi` programmer drives the device over libusb, so it needs
write access to the `/dev/bus/usb` node rather than a tty. No kernel driver
binds `1a86:5512`, so there is nothing to unbind first. flashrom is already
installed at `/usr/sbin/flashrom`, which is not on a non-root `PATH` here.

## What it shows

Hostname, the active IPv4 address and which interface it is on, CPU
temperature, memory used, 1 minute load average, uptime, and the product
strings of every attached USB device that is not a root hub. Everything comes
from `sysfs`, `procfs` and `getifaddrs()`; there are no runtime dependencies
beyond libc and LVGL, and nothing is shelled out to.

IPv4 only, and interface-agnostic. A Pi with DHCP reservations on both
interfaces still usually has only one of them up, so the panel takes whichever
holds an address and prefers the wired one rather than hardcoding `eth0` or
`wlan0`. IPv6 is deliberately ignored, because nobody is reading a SLAAC
address off a 3.5 inch display.

Labels are only rewritten when their text actually changes. Every redraw is
real milliseconds on a 32 MHz SPI panel, so a 1 Hz refresh of unchanged values
costs nothing.

REBOOT and SHUTDOWN arm on the first tap and fire on the second, reverting
after 3 seconds. A resistive touchscreen on a bench picks up stray contact,
and one brush past the panel should not power the machine off.

Idle cost is about 2.8 MB RSS and 0.0% CPU.

## Notes

* LVGL 9 talks to the framebuffer through `lv_linux_fbdev_create()` /
  `lv_linux_fbdev_set_file()` and to the touchscreen through
  `lv_evdev_create()`. The LVGL 8 display/input driver API is gone; examples
  written for it will not compile.
* `lv_conf.h` only lists the settings we change. `lv_conf_internal.h` supplies
  a default for every option we leave out.
* `fbtft` pushes the framebuffer to the panel over SPI via deferred IO. If
  updates do not appear, try `lv_linux_fbdev_set_force_refresh(disp, true)`,
  or set `LV_LINUX_FBDEV_MMAP 0` to use `pwrite()` instead of `mmap`.

### The console owns fb0 too

`vtcon1` is bound to the same framebuffer, and it will corrupt the UI. What we
saw: large rectangles of the screen reverting to pure black, with only the
regions LVGL had recently invalidated still showing the right colours.

The cause is that `fbtft` does not implement `fb_blank`. When the console
blanks the display it therefore falls back to *software* blanking, which means
painting black straight into the framebuffer. `/sys/class/graphics/fb0/blank`
reads `4` (`FB_BLANK_POWERDOWN`) while the panel is visibly showing a UI.
LVGL notices and tries to undo it, which is what this startup warning is:

```
ioctl(FBIOBLANK): Invalid argument
```

That is LVGL issuing `FB_BLANK_UNBLANK`, and fbtft answering `EINVAL`. It is
harmless in itself, but it means LVGL cannot recover the screen on its own.
Because the default render mode is `LV_DISPLAY_RENDER_MODE_PARTIAL`, LVGL only
repaints areas it has invalidated, so the blacked-out regions stay black.
Note that `consoleblank=0` on the kernel command line does not prevent this.

`gpm` is a second offender. It ships enabled on this image, running
`gpm -m /dev/input/mice -t exps2`, and `/dev/input/mice` aggregates `mouse0`,
which is the ADS7846. So it reads the touchscreen as a mouse and paints a
console cursor onto the framebuffer.

### Debugging notes

* The journal is not persistent. `/var/log/journal/` exists but is empty, so
  journald writes to `/run` and every reboot destroys the evidence. Worth
  fixing before chasing any crash:
  `sudo mkdir -p /var/log/journal && sudo systemctl restart systemd-journald`
* `systemd` arms the BCM2835 hardware watchdog with a 60 second timeout, so
  anything that wedges PID 1 is a hard reset with no log at all.
* `sudo` here is not passwordless. The RPi image sets
  `Defaults timestamp_type=global` in `/etc/sudoers.d/010_global-tty`, so a
  `sudo` in any session warms the credential cache for every other session by
  the same user.
* To screenshot the panel, dump the framebuffer and decode RGB565:
  `cat /dev/fb0 > fb.raw` gives 307200 bytes, 480x320 at 2 bytes per pixel
  with no stride padding. Counting distinct pixel values is a fast corruption
  check; a clean screen has well under a hundred.
* The display is `spi0.0` and the touchscreen is `spi0.1`, sharing one SPI
  bus at 32 MHz with `fps=30`.
