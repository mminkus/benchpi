/* benchpi: a touchscreen status panel for the bench Pi.
 *
 * Draws straight to the ILI9486 framebuffer and reads the ADS7846 touchscreen
 * as an evdev pointer. No X11, no Wayland, no compositor.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#define FB_DEV      "/dev/fb0"
#define TOUCH_DEV   "/dev/input/event1"

/* Cap the idle sleep so a lv_timer_handler() of LV_NO_TIMER_READY does not
 * park us for 49 days, and so touch stays responsive at ~30fps. */
#define MAX_IDLE_MS 33

/* How long a destructive button stays armed after the first tap. */
#define ARM_MS      3000

#define C_BG        0x0d0f13
#define C_PANEL     0x161a21
#define C_HEADER    0x11161d
#define C_ACCENT    0x22d3ee
#define C_TEXT      0xe6e8eb
#define C_DIM       0x78828f

static lv_obj_t * lbl_ip;
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_ram;
static lv_obj_t * lbl_load;
static lv_obj_t * lbl_uptime;
static lv_obj_t * lbl_usb;

/* ------------------------------------------------------------------ system */

/* First line of a file, newline stripped. NULL if it cannot be read. */
static char * slurp(const char * path, char * buf, size_t n)
{
    FILE * f = fopen(path, "r");
    if(f == NULL) return NULL;
    char * r = fgets(buf, n, f);
    fclose(f);
    if(r == NULL) return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

/* Prefer a wired address, fall back to any non-loopback IPv4. Nobody is
 * reading a SLAAC address off a 3.5 inch panel, so v6 is deliberately
 * ignored. */
static void current_ip(char * out, size_t n)
{
    struct ifaddrs * list;
    struct ifaddrs * ifa;
    char name[IF_NAMESIZE] = "";
    char addr[INET_ADDRSTRLEN] = "";

    if(getifaddrs(&list) != 0) {
        snprintf(out, n, "no link");
        return;
    }

    for(ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
        if(ifa->ifa_addr == NULL) continue;
        if(ifa->ifa_addr->sa_family != AF_INET) continue;
        if(ifa->ifa_flags & IFF_LOOPBACK) continue;
        if(!(ifa->ifa_flags & IFF_UP)) continue;

        /* Keep the first match, but let a wired interface displace it. */
        if(addr[0] != '\0' && strncmp(ifa->ifa_name, "eth", 3) != 0) continue;

        inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                  addr, sizeof addr);
        snprintf(name, sizeof name, "%s", ifa->ifa_name);
        if(strncmp(ifa->ifa_name, "eth", 3) == 0) break;
    }
    freeifaddrs(list);

    if(addr[0] != '\0') snprintf(out, n, "%s  %s", name, addr);
    else                snprintf(out, n, "no link");
}

static int mem_used_pct(void)
{
    char line[128];
    unsigned long total = 0, avail = 0, v;

    FILE * f = fopen("/proc/meminfo", "r");
    if(f == NULL) return -1;
    while(fgets(line, sizeof line, f) != NULL) {
        if(sscanf(line, "MemTotal: %lu", &v) == 1) total = v;
        else if(sscanf(line, "MemAvailable: %lu", &v) == 1) avail = v;
        if(total != 0 && avail != 0) break;
    }
    fclose(f);

    return total != 0 ? (int)((total - avail) * 100 / total) : -1;
}

static void fmt_uptime(char * out, size_t n)
{
    char buf[64];
    double secs = 0;

    if(slurp("/proc/uptime", buf, sizeof buf) != NULL) secs = atof(buf);

    unsigned t = (unsigned)secs;
    unsigned d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;

    if(d != 0)      snprintf(out, n, "up %ud %uh %um", d, h, m);
    else if(h != 0) snprintf(out, n, "up %uh %um", h, m);
    else            snprintf(out, n, "up %um", m);
}

/* Product strings of everything on the USB bus that is not a root hub. */
static void usb_devices(char * out, size_t n)
{
    struct dirent ** ents;
    size_t used = 0;

    out[0] = '\0';

    int cnt = scandir("/sys/bus/usb/devices", &ents, NULL, alphasort);
    if(cnt < 0) {
        snprintf(out, n, "(cannot read sysfs)");
        return;
    }

    for(int i = 0; i < cnt; i++) {
        char path[512], product[128], vendor[32];

        snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idVendor", ents[i]->d_name);
        if(slurp(path, vendor, sizeof vendor) != NULL && strcmp(vendor, "1d6b") == 0) {
            continue;   /* Linux Foundation root hub, not a real device */
        }
        snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/product", ents[i]->d_name);
        if(slurp(path, product, sizeof product) == NULL) continue;

        int wrote = snprintf(out + used, n - used, "%s%s", used != 0 ? "\n" : "", product);
        if(wrote < 0 || (size_t)wrote >= n - used) break;
        used += (size_t)wrote;
    }

    for(int i = 0; i < cnt; i++) free(ents[i]);
    free(ents);

    if(out[0] == '\0') snprintf(out, n, "(none attached)");
}

/* ------------------------------------------------------------------ actions */

typedef struct {
    const char * caption;
    const char * cmd;
    uint32_t     colour;
    lv_obj_t   * btn;
    lv_obj_t   * label;
    lv_timer_t * disarm;    /* non-NULL only while armed */
} action_t;

/* Absolute paths so the sudoers rule matches exactly, and -n so a missing
 * rule fails immediately rather than blocking on a password prompt that
 * nobody can answer from a touchscreen. */
static action_t actions[] = {
    { "REBOOT",   "sudo -n /usr/bin/systemctl reboot",   0xb45309, NULL, NULL, NULL },
    { "SHUTDOWN", "sudo -n /usr/bin/systemctl poweroff", 0xb91c1c, NULL, NULL, NULL },
};

static void disarm_cb(lv_timer_t * t)
{
    action_t * a = lv_timer_get_user_data(t);

    a->disarm = NULL;   /* repeat count is 1, so LVGL deletes the timer itself */
    lv_label_set_text(a->label, a->caption);
    lv_obj_set_style_text_color(a->label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(a->btn, lv_color_hex(a->colour), 0);
}

/* Two taps, because one stray brush against a resistive panel should not
 * power the bench off mid-session. */
static void action_pressed(lv_event_t * e)
{
    action_t * a = lv_event_get_user_data(e);

    if(a->disarm == NULL) {
        lv_label_set_text(a->label, "CONFIRM?");
        lv_obj_set_style_text_color(a->label, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_color(a->btn, lv_color_hex(0xfbbf24), 0);
        a->disarm = lv_timer_create(disarm_cb, ARM_MS, a);
        lv_timer_set_repeat_count(a->disarm, 1);
        return;
    }

    lv_timer_delete(a->disarm);
    a->disarm = NULL;
    lv_label_set_text(a->label, "...");
    lv_refr_now(NULL);      /* get it on the panel before systemd kills us */

    if(system(a->cmd) != 0) lv_label_set_text(a->label, "NO SUDO");
}

/* ------------------------------------------------------------------ widgets */

static lv_obj_t * panel(lv_obj_t * parent, int x, int y, int w, int h)
{
    lv_obj_t * p = lv_obj_create(parent);

    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_pad_all(p, 6, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    return p;
}

static lv_obj_t * label(lv_obj_t * parent, const lv_font_t * font, uint32_t colour,
                        lv_align_t align, int x, int y, const char * text)
{
    lv_obj_t * l = lv_label_create(parent);

    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_align(l, align, x, y);

    return l;
}

/* Repainting an unchanged label still dirties the area, and every redraw is
 * real milliseconds on a 32 MHz SPI panel. */
static void set_text(lv_obj_t * l, const char * s)
{
    if(strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}

static void build_ui(void)
{
    char host[64];

    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: who we are, and how to reach us. */
    lv_obj_t * hdr = panel(scr, 0, 0, 480, 34);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_radius(hdr, 0, 0);

    if(gethostname(host, sizeof host) != 0) snprintf(host, sizeof host, "benchpi");
    host[sizeof host - 1] = '\0';
    label(hdr, &lv_font_montserrat_20, C_ACCENT, LV_ALIGN_LEFT_MID, 0, 0, host);
    lbl_ip = label(hdr, &lv_font_montserrat_20, C_TEXT, LV_ALIGN_RIGHT_MID, 0, 0, "...");

    /* Three stat tiles: 6px gutters, 152px each, exactly filling 480. */
    static const char * caption[] = { "CPU TEMP", "MEMORY", "LOAD" };
    lv_obj_t ** value[] = { &lbl_temp, &lbl_ram, &lbl_load };
    for(int i = 0; i < 3; i++) {
        lv_obj_t * tile = panel(scr, 6 + i * 158, 40, 152, 62);
        *value[i] = label(tile, &lv_font_montserrat_28, C_TEXT, LV_ALIGN_TOP_MID, 0, -2, "--");
        label(tile, &lv_font_montserrat_14, C_DIM, LV_ALIGN_BOTTOM_MID, 0, 0, caption[i]);
    }

    lv_obj_t * usb = panel(scr, 6, 108, 468, 132);
    label(usb, &lv_font_montserrat_14, C_DIM, LV_ALIGN_TOP_LEFT, 0, 0, "USB");
    lbl_uptime = label(usb, &lv_font_montserrat_14, C_DIM, LV_ALIGN_TOP_RIGHT, 0, 0, "");
    lbl_usb = label(usb, &lv_font_montserrat_20, C_TEXT, LV_ALIGN_TOP_LEFT, 0, 22, "");

    for(int i = 0; i < 2; i++) {
        action_t * a = &actions[i];

        a->btn = lv_button_create(scr);
        lv_obj_set_pos(a->btn, 6 + i * 240, 246);
        lv_obj_set_size(a->btn, 228, 68);
        lv_obj_set_style_bg_color(a->btn, lv_color_hex(a->colour), 0);
        lv_obj_set_style_radius(a->btn, 4, 0);
        lv_obj_add_event_cb(a->btn, action_pressed, LV_EVENT_CLICKED, a);

        a->label = label(a->btn, &lv_font_montserrat_28, 0xffffff,
                         LV_ALIGN_CENTER, 0, 0, a->caption);
    }
}

static void refresh(lv_timer_t * t)
{
    char buf[1024], tmp[64];

    LV_UNUSED(t);

    current_ip(buf, sizeof buf);
    set_text(lbl_ip, buf);

    if(slurp("/sys/class/thermal/thermal_zone0/temp", tmp, sizeof tmp) != NULL) {
        snprintf(buf, sizeof buf, "%.1fC", atoi(tmp) / 1000.0);
    }
    else {
        snprintf(buf, sizeof buf, "--");
    }
    set_text(lbl_temp, buf);

    int pct = mem_used_pct();
    if(pct >= 0) snprintf(buf, sizeof buf, "%d%%", pct);
    else         snprintf(buf, sizeof buf, "--");
    set_text(lbl_ram, buf);

    if(slurp("/proc/loadavg", tmp, sizeof tmp) != NULL) {
        char * sp = strchr(tmp, ' ');
        if(sp != NULL) *sp = '\0';
        set_text(lbl_load, tmp);
    }

    fmt_uptime(buf, sizeof buf);
    set_text(lbl_uptime, buf);

    usb_devices(buf, sizeof buf);
    set_text(lbl_usb, buf);
}

int main(void)
{
    lv_init();

    lv_display_t * disp = lv_linux_fbdev_create();
    if(lv_linux_fbdev_set_file(disp, FB_DEV) != LV_RESULT_OK) {
        fprintf(stderr, "cannot open %s\n", FB_DEV);
        return 1;
    }

    if(lv_evdev_create(LV_INDEV_TYPE_POINTER, TOUCH_DEV) == NULL) {
        fprintf(stderr, "cannot open %s\n", TOUCH_DEV);
        return 1;
    }

    build_ui();
    refresh(NULL);                          /* populate before the first tick */
    lv_timer_create(refresh, 1000, NULL);

    for(;;) {
        uint32_t idle = lv_timer_handler();
        usleep((idle > MAX_IDLE_MS ? MAX_IDLE_MS : idle) * 1000);
    }
}
