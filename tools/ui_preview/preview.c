/* Headless render of the AirPods page (app/aap_ctl/aap_ui.c) on the desktop, for design review.
 *   preview <out.ppm> <scenario> <height> [scroll_y]
 * Scenarios are mock hb_aap_state_t values; the page code is exactly what runs on the nano. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl/lvgl.h"
#include "aap_ui.h"

static uint32_t g_now = 90000;
static uint32_t host_ms(void) { return g_now; }
void aap_ui_preview_notice(uint32_t until);
int aap_ui_snap_active(void);
int aap_ui_present_tick(void);
void aap_ui_set_present(void (*f)(const void *, int, int, int));
static uint8_t *g_fb; static int g_w, g_h;
#ifdef AAP_UI_BENCH
static uint32_t host_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000u); }
static void host_log(const char *tag, uint32_t a, uint32_t b) { printf("%s avg %u us  max %u us\n", tag, a, b); }
#endif

static uint8_t *g_buf;       /* the draw buffer: DIRECT mode like the device, so only dirty areas are redrawn */
static long g_flush_px, g_flush_n; static int g_flush_log;
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    int w = a->x2 - a->x1 + 1;
    g_flush_px += (long)w * (a->y2 - a->y1 + 1); g_flush_n++;
    if (g_flush_log) printf("    flush %d,%d..%d,%d  (%d x %d)\n", a->x1, a->y1, a->x2, a->y2, w, a->y2 - a->y1 + 1);
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(g_fb + ((size_t)y * g_w + a->x1) * 4, g_buf + ((size_t)y * g_w + a->x1) * 4, (size_t)w * 4);
    lv_display_flush_ready(d);
}

static int g_px, g_py, g_down;
static void indev_cb(lv_indev_t *i, lv_indev_data_t *d) { (void)i; d->point.x = g_px; d->point.y = g_py; d->state = g_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static hb_aap_state_t st;
static void step(int ms) { g_now += (uint32_t)ms; aap_ui_update(&st, g_now); lv_timer_handler(); }
static void dump(const char *base, const char *tag)
{
    char path[512]; snprintf(path, sizeof path, "%s.%s.ppm", base, tag);
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) { fputc(g_fb[i * 4 + 2], f); fputc(g_fb[i * 4 + 1], f); fputc(g_fb[i * 4 + 0], f); }
    fclose(f);
}

/* the desktop twin of the device's hb_lv_present565: RGB565 rows -> the "display" */
static void host_present(const void *pic, int stride, int pic_h, int sy)
{
    if (sy + g_h > pic_h) sy = pic_h - g_h;
    for (int y = 0; y < g_h; y++) {
        const uint16_t *s = (const uint16_t *)((const uint8_t *)pic + (size_t)(sy + y) * stride);
        uint32_t *o = (uint32_t *)g_fb + (size_t)y * g_w;
        for (int x = 0; x < g_w; x++) {
            uint32_t p = s[x];
            o[x] = 0xff000000u | ((((p >> 11) * 2106) >> 8) << 16) | (((((p >> 5) & 63) * 1037) >> 8) << 8) | (((p & 31) * 2106) >> 8);   /* LVGL's rounding */
        }
    }
}

static void setstr(char *d, size_t n, const char *s) { strncpy(d, s, n - 1); d[n - 1] = 0; }

static void scenario(int n)
{
    memset(&st, 0, sizeof st);
    st.magic = HB_AAP_SHARED_MAGIC; st.abi = HB_AAP_SHARED_ABI; st.size = sizeof st;
    st.batt_l = st.batt_r = st.batt_c = -1; st.le_l = st.le_r = st.le_c = -1; st.mode_seen = -1;
    memset((void *)st.ctl_val, 0xff, sizeof st.ctl_val); memset((void *)st.ctl_val2, 0xff, sizeof st.ctl_val2);
    st.le_variant = 2; st.le_scan_rc = 0; st.le_raw = 41; st.le_adv = 5; st.le_scans = 2;
    switch (n) {
    case 0:   /* Pro 2 (USB-C), connected, noise cancellation, bud charging in case */
        st.cid = 5; st.rx_count = 30; st.mode_seen = 2;
        st.batt_l = 83; st.st_l = 2; st.batt_r = 100; st.st_r = 1; st.batt_c = 57; st.st_c = 2;
        st.le_model = 0x2420; st.key_state = 1;
        setstr(st.dev_name, sizeof st.dev_name, "Alex's AirPods Pro"); setstr(st.dev_model, sizeof st.dev_model, "A3048");
        setstr(st.dev_serial, sizeof st.dev_serial, "F4KE5N0AAAAA"); setstr(st.dev_fw, sizeof st.dev_fw, "8A358");
        st.ctl_val[CTL_CONV_DETECT] = 2; st.ctl_val[CTL_ONE_BUD_NC] = 1;   /* ear detection is not announced */
        st.link_avdtp = 2;
        break;
    case 1:   /* Pro (1st gen), transparency, case last known */
        st.cid = 5; st.rx_count = 30; st.mode_seen = 3;
        st.batt_l = 92; st.st_l = 2; st.batt_r = 100; st.st_r = 2; st.batt_c = 71; st.st_c = 4;
        setstr(st.dev_name, sizeof st.dev_name, "Jordan's AirPods Pro"); setstr(st.dev_model, sizeof st.dev_model, "A2084");
        setstr(st.dev_serial, sizeof st.dev_serial, "F4KE5N0BBBBB"); setstr(st.dev_fw, sizeof st.dev_fw, "4E71");
        st.ctl_val[CTL_EAR_DETECT] = 1; st.ctl_val[CTL_ONE_BUD_NC] = 2; st.ctl_val[CTL_CLICK_HOLD] = 1; st.ctl_val2[CTL_CLICK_HOLD] = 5;
        break;
    case 2:   /* AirPods 3, basic */
        st.cid = 5; st.rx_count = 12;
        st.batt_l = 45; st.st_l = 2; st.batt_r = 12; st.st_r = 2; st.batt_c = 100; st.st_c = 1;
        setstr(st.dev_name, sizeof st.dev_name, "Sam's AirPods"); setstr(st.dev_model, sizeof st.dev_model, "A2565");
        st.ctl_val[CTL_EAR_DETECT] = 2;
        break;
    case 3:   /* AirPods Max */
        st.cid = 5; st.rx_count = 12; st.mode_seen = 1; st.batt_l = 64; st.st_l = 2;
        st.le_model = 0x0a20;
        setstr(st.dev_name, sizeof st.dev_name, "Studio Max"); setstr(st.dev_model, sizeof st.dev_model, "A2096");
        st.ctl_val[CTL_EAR_DETECT] = 1;
        break;
    case 7:   /* Pro 2 again, with the "did not accept" note showing (see aap_ui_preview_notice) */
        st.cid = 5; st.rx_count = 30; st.mode_seen = 4;
        st.batt_l = 100; st.st_l = 1; st.batt_r = 100; st.st_r = 1; st.batt_c = 100; st.st_c = 1;
        st.le_model = 0x2420;
        setstr(st.dev_name, sizeof st.dev_name, "Alex's AirPods Pro"); setstr(st.dev_model, sizeof st.dev_model, "A3048");
        setstr(st.dev_serial, sizeof st.dev_serial, "F4KE5N0AAAAA"); setstr(st.dev_fw, sizeof st.dev_fw, "8A358");
        st.ctl_val[CTL_CONV_DETECT] = 2; st.ctl_val[CTL_ONE_BUD_NC] = 1;
        st.link_avdtp = 2;
        break;
    case 8:   /* AirPods Pro 3 (art set 7) */
        st.cid = 5; st.rx_count = 30; st.mode_seen = 3; st.le_model = 0x2720;
        st.batt_l = 64; st.st_l = 2; st.batt_r = 71; st.st_r = 2; st.batt_c = 38; st.st_c = 1;
        setstr(st.dev_name, sizeof st.dev_name, "Alex's AirPods"); setstr(st.dev_model, sizeof st.dev_model, "A3063");
        st.ctl_val[CTL_CONV_DETECT] = 1; st.ctl_val[CTL_ONE_BUD_NC] = 1; st.ctl_val[CTL_EAR_DETECT] = 1;
        break;
    case 9:   /* a model the table does not know: drawn stand-ins */
        st.cid = 5; st.rx_count = 30;
        st.batt_l = 55; st.st_l = 2; st.batt_r = 56; st.st_r = 2; st.batt_c = 90; st.st_c = 2;
        setstr(st.dev_name, sizeof st.dev_name, "My Buds"); setstr(st.dev_model, sizeof st.dev_model, "A9999");
        break;
    case 10:  /* AirPods (1st generation): no LED in the render */
        st.cid = 5; st.rx_count = 12; st.le_model = 0x0220;
        st.batt_l = 20; st.st_l = 2; st.batt_r = 18; st.st_r = 2; st.batt_c = 60; st.st_c = 2;
        setstr(st.dev_name, sizeof st.dev_name, "AirPods"); setstr(st.dev_model, sizeof st.dev_model, "A1523");
        break;
    case 11:  /* AirPods (2nd generation) */
        st.cid = 5; st.rx_count = 12; st.le_model = 0x0f20;
        st.batt_l = 76; st.st_l = 2; st.batt_r = 80; st.st_r = 2; st.batt_c = 44; st.st_c = 2;
        setstr(st.dev_name, sizeof st.dev_name, "AirPods"); setstr(st.dev_model, sizeof st.dev_model, "A2032");
        break;
    case 12:  /* AirPods 4 */
        st.cid = 5; st.rx_count = 12; st.le_model = 0x1920;
        st.batt_l = 90; st.st_l = 2; st.batt_r = 88; st.st_r = 2; st.batt_c = 70; st.st_c = 2;
        setstr(st.dev_name, sizeof st.dev_name, "AirPods 4"); setstr(st.dev_model, sizeof st.dev_model, "A3050");
        break;
    case 4:   /* no AirPods link at all: the empty state */
        break;
    case 6:   /* A2DP is up, the AAP session is not yet: "Connecting..." */
        st.link_avdtp = 2;
        break;
    case 5:   /* BLE advertisement only, no link: still the empty state */
        st.le_model = 0x2420; st.le_l = 80; st.le_r = 90; st.le_c = 60; st.le_flags = LE_LID_OPEN | LE_CHG_L;
        st.le_seen_ms = 89000; st.le_case_ms = 89000;
        break;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: preview out.ppm scenario height [scroll_y]\n"); return 2; }
    int sc = atoi(argv[2]); g_w = 240; g_h = atoi(argv[3]);
    int scroll = argc > 4 ? atoi(argv[4]) : 0;
    g_fb = calloc((size_t)g_w * g_h, 4);
    lv_init();
    lv_tick_set_cb(host_ms);
    lv_display_t *d = lv_display_create(g_w, g_h);
    g_buf = malloc((size_t)g_w * g_h * 4);
    lv_display_set_buffers(d, g_buf, NULL, (uint32_t)((size_t)g_w * g_h * 4), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);
    scenario(sc);
    static const aap_ui_ops_t ops = { 0 };
    aap_ui_build(lv_screen_active(), g_w, g_h, &ops);
    if (sc == 7) aap_ui_preview_notice(host_ms() + 3000);
    aap_ui_update(&st, host_ms());
#ifdef AAP_UI_BENCH
    if (getenv("BENCH")) {                    /* run the on-device benchmark logic on the desktop (relative costs only) */
        extern int aap_ui_bench_step(uint32_t (*us)(void), void (*log)(const char *, uint32_t, uint32_t));
        int n = 0; while (aap_ui_bench_step(host_us, host_log) && ++n < 100000) { }
    }
#endif
    if (getenv("FLINGTEST")) {                  /* how far does a flick carry? FLINGTEST="period_ms throw_pct" */
        int period = 30, thr = 10; sscanf(getenv("FLINGTEST"), "%d %d", &period, &thr);
        lv_indev_t *in = lv_indev_create();
        lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(in, indev_cb);
        lv_indev_set_scroll_limit(in, 10);
        lv_timer_set_period(lv_indev_get_read_timer(in), period);
        lv_indev_set_scroll_throw(in, (uint8_t)thr);
        lv_obj_t *pg = lv_obj_get_child(lv_screen_active(), 0);
        { lv_obj_t *tall = lv_obj_create(pg); lv_obj_remove_style_all(tall); lv_obj_set_pos(tall, 0, 4000); lv_obj_set_size(tall, 1, 1); lv_obj_remove_flag(tall, LV_OBJ_FLAG_CLICKABLE); }   /* a long page: no clamping at the end */
        for (int i = 0; i < 30; i++) step(100);
        static const int speeds[] = { 1, 2, 3 };       /* finger speed, px per 4 ms */
        for (int k = 0; k < 3; k++) {
            lv_obj_scroll_to_y(pg, 0, LV_ANIM_OFF); step(100);
            int v = speeds[k], y = 420, t = 0;
            g_px = 120; g_py = y; g_down = 1; step(period);
            for (; t < 60; t += 4) { y -= v; if (y < 5) y = 5; g_py = y; g_now += 4; aap_ui_update(&st, g_now); lv_timer_handler(); }
            int at_release = (int)lv_obj_get_scroll_y(pg);
            g_down = 0; int end_t = 0;
            for (int i = 0; i < 1500; i++) { step(2); end_t += 2; if (!lv_indev_get_scroll_obj(in)) break; }
            printf("period %2d ms, throw %2d%%: finger %d px/4ms -> at release %d px, flings on to %d px (%d more) in %d ms\n",
                   period, thr, v, at_release, (int)lv_obj_get_scroll_y(pg), (int)lv_obj_get_scroll_y(pg) - at_release, end_t);
        }
        return 0;
    }
    if (getenv("ANIMTEST")) {                   /* what does a mode switch (the sliding pill) redraw, frame by frame? */
        for (int i = 0; i < 30; i++) step(100);
        printf("mode now %d; switching to Transparency (3)\n", (int)st.mode_seen);
        st.mode_seen = 3;
        g_flush_log = 1;
        for (int f = 0; f < 24; f++) { printf("  frame %d\n", f); step(8); }
        g_flush_log = 0;
        return 0;
    }
    if (getenv("EDGETEST")) {                   /* drag past the top and past the bottom: is there any overscroll / bounce? */
        lv_indev_t *in = lv_indev_create();
        lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(in, indev_cb);
        lv_indev_set_scroll_limit(in, 10);
        lv_obj_t *pg = lv_obj_get_child(lv_screen_active(), 0);
        for (int i = 0; i < 30; i++) step(100);
        printf("top: drag DOWN 200 px at the top\n");
        g_px = 120; g_py = 150; g_down = 1; step(30);
        for (int y = 150; y <= 350; y += 10) { g_py = y; step(30); printf("  y=%d page %d screen %d\n", y, (int)lv_obj_get_scroll_y(pg), (int)lv_obj_get_scroll_y(lv_screen_active())); }
        g_down = 0; for (int i = 0; i < 40; i++) { step(30); if (i % 8 == 0) printf("  settle page %d screen %d\n", (int)lv_obj_get_scroll_y(pg), (int)lv_obj_get_scroll_y(lv_screen_active())); }
        return 0;
    }
    if (getenv("SCROLLTEST")) {                 /* drag the page with a simulated finger: the cache must swap in and out cleanly */
        extern void aap_ui_snap_stats(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
        lv_indev_t *in = lv_indev_create();
        lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(in, indev_cb);
        lv_indev_set_scroll_limit(in, 10);
        lv_obj_t *pg = lv_obj_get_child(lv_screen_active(), 0);
        int fast = getenv("FASTPATH") != NULL;
        if (fast) aap_ui_set_present(host_present);
        printf("path: %s\n", fast ? "direct present (device)" : "image swap");
        for (int i = 0; i < 30; i++) step(100);            /* idle: the picture gets drawn after ~400 ms */
        uint32_t tk, fl, ms, fb; aap_ui_snap_stats(&tk, &fl, &ms, &fb);
        printf("snapshots taken %u failed %u last %u ms, biggest free block %u\n", tk, fl, ms, fb);
        g_px = 120; g_py = 400; g_down = 1; step(30);
        for (int y = 400; y >= 250; y -= 10) { g_py = y; step(30); if (fast) aap_ui_present_tick(); }
        step(30); if (fast) aap_ui_present_tick();
        printf("during the drag: snapshot on screen = %d, scroll_y = %d\n", aap_ui_snap_active(), (int)lv_obj_get_scroll_y(pg));
        dump(argv[1], "A");                                 /* a frame drawn from the picture */
        uint8_t *a = malloc((size_t)g_w * g_h * 4); memcpy(a, g_fb, (size_t)g_w * g_h * 4);
        if (fast) {                                         /* LVGL must not draw while the picture is presented */
            for (int i = 0; i < 5; i++) step(30);
            printf("LVGL left the presented frame alone: %s\n", memcmp(a, g_fb, (size_t)g_w * g_h * 4) == 0 ? "yes" : "NO");
        }
        lv_obj_send_event(pg, LV_EVENT_SCROLL_END, NULL);   /* swap back to the live page at the same scroll position */
        step(30);
        printf("after the swap: snapshot on screen = %d, scroll_y = %d\n", aap_ui_snap_active(), (int)lv_obj_get_scroll_y(pg));
        int diff = 0, big = 0, maxd = 0;
        for (int i = 0; i < g_w * g_h; i++)
            for (int c = 0; c < 3; c++) { int d = abs((int)a[i * 4 + c] - (int)g_fb[i * 4 + c]); if (d) { diff++; if (d > 12) big++; if (d > maxd) maxd = d; } }
        printf("picture vs live at the same scroll: %d channel values differ, %d by more than 12, largest %d\n", diff, big, maxd);
        dump(argv[1], "B");                                 /* the live page, same scroll */
        g_down = 0;
        for (int i = 0; i < 80; i++) step(30);              /* release, let any fling finish */
        printf("after the fling: snapshot on screen = %d, scroll_y = %d\n", aap_ui_snap_active(), (int)lv_obj_get_scroll_y(pg));
        return 0;
    }
    lv_obj_t *root = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_update_layout(root);
    if (scroll) lv_obj_scroll_to_y(root, scroll, LV_ANIM_OFF);
    lv_refr_now(NULL);
    lv_timer_handler();
    lv_refr_now(NULL);
    FILE *f = fopen(argv[1], "wb");
    fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) { fputc(g_fb[i * 4 + 2], f); fputc(g_fb[i * 4 + 1], f); fputc(g_fb[i * 4 + 0], f); }
    fclose(f);
    return 0;
}
