/* benchpi proof of concept.
 *
 * Draws straight to the ILI9486 framebuffer and reads the ADS7846 touchscreen
 * as an evdev pointer. No X11, no Wayland, no compositor.
 */
#include <stdio.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#define FB_DEV      "/dev/fb0"
#define TOUCH_DEV   "/dev/input/event1"

/* Cap the idle sleep so a lv_timer_handler() of LV_NO_TIMER_READY does not
 * park us for 49 days, and so touch stays responsive at ~30fps. */
#define MAX_IDLE_MS 33

static lv_obj_t * xy_label;
static lv_obj_t * count_label;
static lv_indev_t * touch;
static unsigned presses;

static void btn_clicked(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_label_set_text_fmt(count_label, "Presses: %u", ++presses);
}

static void build_ui(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), 0);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL TEST");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00d0ff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    xy_label = lv_label_create(scr);
    lv_label_set_text(xy_label, "X: ---  Y: ---");
    lv_obj_set_style_text_font(xy_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(xy_label, lv_color_hex(0xc0c0c8), 0);
    lv_obj_align(xy_label, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_set_size(btn, 260, 84);
    lv_obj_center(btn);
    lv_obj_add_event_cb(btn, btn_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "TOUCH ME");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(btn_label);

    count_label = lv_label_create(scr);
    lv_label_set_text(count_label, "Presses: 0");
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(count_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(count_label, LV_ALIGN_BOTTOM_MID, 0, -14);
}

int main(void)
{
    lv_point_t last = { -1, -1 };

    lv_init();

    lv_display_t * disp = lv_linux_fbdev_create();
    if(lv_linux_fbdev_set_file(disp, FB_DEV) != LV_RESULT_OK) {
        fprintf(stderr, "cannot open %s\n", FB_DEV);
        return 1;
    }

    touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, TOUCH_DEV);
    if(touch == NULL) {
        fprintf(stderr, "cannot open %s\n", TOUCH_DEV);
        return 1;
    }

    build_ui();

    for(;;) {
        lv_point_t p;
        uint32_t idle;

        /* Only repaint the readout when it actually moved. The panel is on SPI,
         * every redraw costs real milliseconds. */
        lv_indev_get_point(touch, &p);
        if(p.x != last.x || p.y != last.y) {
            last = p;
            lv_label_set_text_fmt(xy_label, "X: %3d  Y: %3d", (int)p.x, (int)p.y);
        }

        idle = lv_timer_handler();
        usleep((idle > MAX_IDLE_MS ? MAX_IDLE_MS : idle) * 1000);
    }
}
