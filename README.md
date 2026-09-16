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

## Current state

Proof of concept only. It draws a dark screen with a live touch X/Y readout,
one large `TOUCH ME` button, and a press counter. If the counter increments
when you poke the button, the framebuffer and input plumbing both work and
everything after this is just application code.

## Notes

* LVGL 9 talks to the framebuffer through `lv_linux_fbdev_create()` /
  `lv_linux_fbdev_set_file()` and to the touchscreen through
  `lv_evdev_create()`. The LVGL 8 display/input driver API is gone; examples
  written for it will not compile.
* `lv_conf.h` only lists the settings we change. `lv_conf_internal.h` supplies
  a default for every option we leave out.
* The kernel console (`vtcon1`) is bound to the same framebuffer. It only
  repaints on console activity, but if it fights with the UI, unbind it:
  `echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind`.
* `fbtft` pushes the framebuffer to the panel over SPI via deferred IO. If
  updates do not appear, try `lv_linux_fbdev_set_force_refresh(disp, true)`,
  or set `LV_LINUX_FBDEV_MMAP 0` to use `pwrite()` instead of `mmap`.
