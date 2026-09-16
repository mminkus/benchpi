# pi-dashboard

A touchscreen UI for a Raspberry Pi 5 with a 480x320 ILI9486 SPI LCD hat,
running on the bare Linux framebuffer. No X11, no Wayland, no compositor,
no browser.

## Hardware

| Part      | Detail                                                    |
|-----------|-----------------------------------------------------------|
| Board     | Raspberry Pi 5, 2GB, Debian 13 (trixie), Linux 6.18        |
| Display   | `/dev/fb0`, `fb_ili9486`, 480x320, RGB565, 960 byte stride |
| Touch     | `/dev/input/event1`, ADS7846/XPT2046, `spi0.1`             |

Touch orientation is corrected in the device tree with `swapxy,invy`, so the
evdev coordinates already line up with the landscape framebuffer.

`martin` is in the `video` and `input` groups, so the app does not need root.

## Building

LVGL is not packaged in Debian, so the Makefile fetches the release tarball.
It is not checked in.

```sh
make lvgl        # download LVGL 9.5.0 into ./lvgl
make -j4         # ~700 translation units, a couple of minutes on a Pi 5
./benchpi
```

`make lvgl` has to run first: the source list is a `find` expanded when the
Makefile is parsed.

## Running

The kernel console has to be taken off the framebuffer first, or it will
fight the UI. See "The console owns fb0 too" below.

```sh
sudo systemctl disable --now gpm      # reads the touchscreen as a mouse
echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind
./benchpi
```

`echo 1` to that same file gives the console back.

## Current state

Proof of concept, and it works. Dark screen, live touch X/Y readout, one large
`TOUCH ME` button, a press counter. Verified on hardware: the counter reached
26, touch coordinates track the finger, and the button renders a pressed
state. That means framebuffer output and touch input are both solved and
everything from here is application code.

Costs 2.6 MB RSS and 0.0% CPU while idle.

## Notes

* LVGL 9 talks to the framebuffer through `lv_linux_fbdev_create()` /
  `lv_linux_fbdev_set_file()` and to the touchscreen through
  `lv_evdev_create()`. The LVGL 8 display/input driver API is gone; examples
  written for it will not compile.
* `lv_conf.h` only lists the settings we change. `lv_conf_internal.h` supplies
  a default for every option we leave out.
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

Unbind the console rather than working around it in the app:

```sh
echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind
```

`gpm` is a second offender. It ships enabled on this image, running
`gpm -m /dev/input/mice -t exps2`, and `/dev/input/mice` aggregates `mouse0`,
which is the ADS7846. So it reads the touchscreen as a mouse and paints a
console cursor onto the framebuffer. `sudo systemctl disable --now gpm`.

### Debugging notes

* The journal is not persistent. `/var/log/journal/` exists but is empty, so
  journald writes to `/run` and every reboot destroys the evidence. Worth
  fixing before chasing any crash.
* `systemd` arms the BCM2835 hardware watchdog with a 60 second timeout, so
  anything that wedges PID 1 is a hard reset with no log at all.
* `sudo` here is not passwordless. The RPi image sets
  `Defaults timestamp_type=global`, so a `sudo` in any session warms the
  credential cache for every other session by the same user.
* To screenshot the panel, dump the framebuffer and decode RGB565:
  `cat /dev/fb0 > fb.raw` gives 307200 bytes, 480x320 at 2 bytes per pixel
  with no stride padding.
* The display is `spi0.0` and the touchscreen is `spi0.1`, sharing one SPI
  bus at 32 MHz with `fps=30`.
* `fbtft` pushes the framebuffer to the panel over SPI via deferred IO. If
  updates do not appear, try `lv_linux_fbdev_set_force_refresh(disp, true)`,
  or set `LV_LINUX_FBDEV_MMAP 0` to use `pwrite()` instead of `mmap`.
