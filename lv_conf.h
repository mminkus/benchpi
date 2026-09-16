/* Minimal LVGL config. Everything not set here falls back to the defaults in
 * lvgl/src/lv_conf_internal.h, so this file only lists what we actually change. */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ILI9486 framebuffer is RGB565. */
#define LV_COLOR_DEPTH          16

/* Plain malloc/str/snprintf instead of LVGL's fixed pool. This is Linux with 2GB. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* /dev/fb0 output, evdev (ADS7846) input. No X, no Wayland, no DRM. */
#define LV_USE_LINUX_FBDEV      1
#define LV_USE_EVDEV            1

#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF           1

/* Default font is 14px, unreadable at arm's length on a 3.5" panel. */
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_28   1

#endif /* LV_CONF_H */
