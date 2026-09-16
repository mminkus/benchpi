# CLAUDE.md

Working notes for this repo. Read before touching the Pi.

## What this is

`benchpi`, an LVGL touchscreen status panel for a Raspberry Pi 5 with a
480x320 ILI9486 SPI hat, drawing straight to the Linux framebuffer. No X11, no
Wayland, no compositor, no browser.

Shows hostname, active IPv4 and its interface, CPU temperature, memory, load,
uptime, attached USB devices, and REBOOT / SHUTDOWN buttons. Everything comes
from sysfs, procfs and `getifaddrs()`. IPv4 only on purpose.

Deliberately not built, because it has not been needed: GPIO, I2C and SPI
pages, a serial console launcher, flashrom controls. Martin does 1-wire on
ESP32s and has not touched I2C on this box. Add when there is a real use.

## Layout

The canonical repo lives on the dev box at `debian.dc` in
`/home/martin/development/pi-dashboard`. The Pi clones from it:

```sh
# on the Pi
git clone martin@10.1.1.90:/home/martin/development/pi-dashboard ~/pi-dashboard
git config core.sshCommand "ssh -i ~/.ssh/martin_at_luna_rsa_key"
```

The Pi already holds `martin_at_luna_rsa_key`, and the dev box authorises
`martin@luna`, so that key is what makes the clone work. Note the dev box is
`10.1.1.90`, a different subnet from the Pi's `10.2.1.60`, but routable.

Edit here, commit here, `git pull` on the Pi, build on the Pi.

## The device tree config that makes the hat work

In `/boot/firmware/config.txt`. This is the hard-won part; nothing else in this
repo matters if these are wrong.

```
dtparam=spi=on

[all]
dtoverlay=fbtft,spi0-0,piscreen,dc_pin=24,reset_pin=25,led_pin=22,rotate=90
dtoverlay=ads7846,cs=1,penirq=17,speed=2000000,swapxy,invy,xmin=500,xmax=3650,ymin=450,ymax=3750
```

Notes on why each piece is there:

- `piscreen` is the fbtft variant that drives this ILI9486. The generic
  `ili9486` overlay is not the same thing.
- `rotate=90` is what makes `/dev/fb0` come up as 480x320 landscape rather
  than 320x480 portrait, so no software rotation is needed anywhere.
- `swapxy,invy` on the touchscreen matches the panel rotation. The evdev
  coordinates therefore already line up with the framebuffer. **Do not add a
  transform in the application.**
- `xmin/xmax/ymin/ymax` are the touch calibration. They are why raw ADS7846
  readings map onto 0..479 and 0..319 correctly.
- `[pi5] dtoverlay=nospi10` is already in the stock file above these lines and
  is required on a Pi 5.

## Hardware facts

| Thing        | Value                                                     |
|--------------|-----------------------------------------------------------|
| Pi           | Pi 5, 2GB, Debian 13 trixie, Linux 6.18, aarch64           |
| Display      | `/dev/fb0`, `fb_ili9486`, 480x320 RGB565, stride 960       |
| Display bus  | `spi0.0` at 32 MHz, `fps=30`, DT `rotate=90`               |
| Touch        | `/dev/input/event1`, ADS7846, `spi0.1`, also `mouse0`      |
| Permissions  | `martin` is in `video` and `input`, so no root needed      |

Touch orientation is already fixed in the device tree (`swapxy,invy`), so
evdev coordinates line up with the landscape framebuffer. Do not add a
software rotation on top of it.

`fbtft` is a staging driver and says so in dmesg. Assume rough edges.

## LVGL

Not packaged in Debian. `make lvgl` fetches the 9.5.0 tarball; it is not
vendored and is gitignored.

LVGL 9 API, which is NOT the LVGL 8 API that most examples online use:

```c
lv_display_t *d = lv_linux_fbdev_create();
lv_linux_fbdev_set_file(d, "/dev/fb0");
lv_indev_t   *t = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
```

Everything is reachable from `lvgl.h`, which pulls in `src/drivers/lv_drivers.h`.
`lv_btn_create` is `lv_button_create`, `lv_scr_act` is `lv_screen_active`.
The fbdev driver installs its own `lv_tick_set_cb`, so do not set one.

`lv_conf.h` only lists settings we change. `lv_conf_internal.h` has 472
`#ifndef` guards supplying a default for everything else, so a short conf file
is legitimate, not an accident.

Build is ~700 translation units, 27 seconds with `make -j4` on the Pi. Fast
enough that there is no reason to set up cross-compiling.

`lv_timer_handler()` can return `LV_NO_TIMER_READY` (`UINT32_MAX`). Clamp the
sleep or the loop parks for 49 days.

## Privileged actions

The UI runs unprivileged. Only the two power buttons need root, and they shell
out to `sudo -n` with absolute paths against `/etc/sudoers.d/benchpi`, shipped
in the repo as `benchpi.sudoers`.

polkit is not an option here: every session on this box is seatless, so
`org.freedesktop.login1.power-off` requires authentication that a touchscreen
cannot supply. `pkcheck --action-id org.freedesktop.login1.power-off --process $$`
returns "Authorization requires authentication". Do not "fix" this by running
the whole UI as root.

`sudo -n` rather than `sudo` matters: without it, a missing sudoers rule makes
the app block forever on a password prompt instead of showing `NO SUDO`.

## Gotchas that cost time

**The console fights you for fb0.** Large rectangles of the screen revert to
pure black while only recently-invalidated regions stay correct. Cause:
`fbtft` does not implement `fb_blank`, so the console software-blanks by
painting black into the framebuffer. `/sys/class/graphics/fb0/blank` reads `4`
(`FB_BLANK_POWERDOWN`) while the panel is visibly showing a UI. LVGL tries to
undo this and fails, which is the `ioctl(FBIOBLANK): Invalid argument` warning
on startup. Default render mode is partial, so LVGL never repaints the damage.

Fix at the root, not in the app:

```sh
sudo systemctl disable --now gpm
echo 0 | sudo tee /sys/class/vtconsole/vtcon1/bind
```

**gpm** was installed to test the touchscreen before this project existed, and
then quietly kept running. `gpm -m /dev/input/mice -t exps2`, and
`/dev/input/mice` aggregates `mouse0`, which is the ADS7846. It read the
touchscreen as a mouse and painted a cursor onto the framebuffer. It has been
`apt purge`d, so `systemctl disable gpm` will now fail with "Unit
gpm.service does not exist". That is the desired state, not a problem.

**sudo is not passwordless.** The image sets `Defaults timestamp_type=global`,
so a `sudo` in any of Martin's sessions warms the credential cache for every
other session by the same user. There is no `NOPASSWD` rule. Do not rely on
it; ask for the commands to be run instead.

**The journal was not persistent**, and the obvious fix does not work.
`Storage=auto` does not mean "write to /var". journald writes to `/run` and
stays there until something flushes it, and that something is
`systemd-journal-flush.service`, a `static` oneshot that runs once at boot.
So `mkdir /var/log/journal && systemctl restart systemd-journald` leaves
journald in the pre-flush state and looks like it did nothing. Either
`systemctl restart systemd-journal-flush`, or just reboot once the directory
exists.

**systemd arms the BCM2835 hardware watchdog at 60 seconds.** Anything that
wedges PID 1 for a minute is a hard reset with no log.

## Bench peripherals

The point of the box. Both were previously used from a MacBook Pro; the plan
is to do this work on the Pi from now on.

| Device | USB ID | Product string | Node |
| --- | --- | --- | --- |
| CH341A programmer | `1a86:5512` | `USB UART-LPT` | none, raw USB |
| FT232R serial | `0403:6001` | `FT232R USB UART` | `/dev/ttyUSB0` |

`martin` is in `dialout`, so the FT232R works with no further setup. The
CH341A is raw USB, so flashrom will want root or a udev rule. **flashrom is
not installed yet.**

The dashboard lists USB devices by reading `product` from
`/sys/bus/usb/devices/*/` and skipping vendor `1d6b`, which is the Linux
Foundation root hubs. There are four of those on a Pi 5.

## Testing without being in the room

Screenshot the panel by dumping the framebuffer and decoding RGB565:

```sh
ssh martin@10.2.1.60 'cat /dev/fb0 > /tmp/fb.raw'   # 307200 bytes, no padding
```

```python
d = np.fromfile('fb.raw', dtype='<u2').reshape(320, 480)
r = ((d >> 11) & 0x1F) * 255 // 31
g = ((d >>  5) & 0x3F) * 255 // 63
b = ( d        & 0x1F) * 255 // 31
```

Counting distinct pixel values in the dump is a fast way to spot corruption.
A clean UI screen has well under a hundred.

The framebuffer survives hard abuse: 20 seconds of full-frame `write()` blits
and 20 seconds of `mmap` writes at ~9600 fps both pass without complaint, so
rule fbtft out early rather than suspecting it.

## Open

One unexplained hard reboot, roughly 20 seconds into the first run of the app.
Not reproducible since; the app has soaked for over ten minutes at 2.6 MB RSS
and 0.0% CPU. Framebuffer stress tests do not trigger it. No log survived,
because of the journal problem above. Watch for it.
