/* aap_ui.c — the AirPods page for the 240x432 screen, in the iOS 16/17 style.
 *
 * Three states, chosen from the shared state:
 *   - no AirPods link        -> an Apple-style empty state: "Connect Your AirPods"
 *   - link, no AAP session   -> "Connecting..." (with Try Again)
 *   - connected              -> the page: name/model/status, buds and case with batteries, NOISE CONTROL,
 *                               SETTINGS (rows a model has), ABOUT. It scrolls.
 *
 * Responsiveness: a tap shows its result at once (the segment or switch moves immediately). The AirPods echo
 * a listening-mode change but never a setting write (they announce settings once, at connect), so the
 * resident records a sent write as the current value; if it could not be sent at all, the switch snaps back
 * and a note says so.
 * Speed: absolute positions, flat shapes, no opacity layers, clipped corners, gradients or LVGL switches. */
#include "aap_ui.h"
#include "aap_models.h"
#include "aap_art.h"
#include "aap_icons.h"
#include "aap_font.h"
#include "aap_snap.h"
#define NC2_BENCH (AF_CASE | AF_EAR_DETECT | AF_PRESS_HOLD | AF_NC_MODES | AF_ONE_BUD_NC | AF_ADAPTIVE | AF_CONV_AWARE | AF_CASE_TONE)
#include <string.h>

/* ---- palette (iOS dark, grouped style) ---- */
/* The page cache (below) holds the page as RGB565. The dark greys of the cards would shift by a few levels
 * (and tint) in that round trip, so they are defined as what RGB565 shows: Q565() truncates to 5/6/5 bits and
 * expands back exactly as LVGL's blend (and hb_lv_present565) does. The live page and the cached picture then match to the pixel. */
#define Q5(v)     (((((v) >> 3) * 2106) >> 8))      /* LVGL's own RGB565 -> 8 bit rounding (also the device presenter's) */
#define Q6(v)     (((((v) >> 2) * 1037) >> 8))
#define Q565(r, g, b) (((uint32_t)Q5(r) << 16) | ((uint32_t)Q6(g) << 8) | (uint32_t)Q5(b))
#define C_BG      0x000000
#define C_CELL    Q565(0x20, 0x20, 0x20)          /* iOS dark card: 1C1C1E, nudged to a grey RGB565 can show */
#define C_SEP     Q565(0x38, 0x38, 0x38)
#define C_TEXT    0xFFFFFF
#define C_DIM     0x8E8E93
#define C_GREEN   0x30D158
#define C_AMBER   0xFF9F0A
#define C_RED     0xFF453A
#define C_BLUE    0x0A84FF
#define C_SEG_SEL Q565(0x63, 0x63, 0x63)
#define C_SW_OFF  Q565(0x39, 0x39, 0x39)

#define F_SMALL   (&aap_inter_14)
#define F_BODY    (&aap_inter_16)
#define F_BIG     (&aap_inter_20)
#define F_SYMBOL  (&lv_font_montserrat_14)     /* the charging bolt is a Font Awesome glyph; Inter has none */

#define M      12                      /* side margin */
#define ROW_H  54                      /* settings row */
#define ART_H  64                      /* height of the picture slot above each battery */
#define HERO_Y 58                      /* top of the buds/case row (under the name and model) */
#define NC_Y   200                     /* top of the NOISE CONTROL block */
static int W, H, CW;
static const aap_ui_ops_t *s_ops;
static const hb_aap_state_t *s_t;      /* latest state and time, for the tap handlers */
static uint32_t s_now;

/* ------------------------------------------------------------------ builders ---- */
static lv_obj_t *mk(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *shape(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t c)
{
    lv_obj_t *o = mk(p, x, y, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* One line, long text ends in "..." (w > 0), or a free label (w == 0). */
static lv_obj_t *lbl(lv_obj_t *p, int x, int y, int w, const char *t, const lv_font_t *f, uint32_t c, lv_text_align_t al)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_text_align(l, al, 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_size(l, w, lv_font_get_line_height(f));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    return l;
}

/* Wrapping text block of fixed width. */
static lv_obj_t *para(lv_obj_t *p, int x, int y, int w, const char *t, const lv_font_t *f, uint32_t c, lv_text_align_t al)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_text_align(l, al, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* LVGL repaints an object on every setter call, even when the value is the same. A page update used to call
 * dozens of setters, so a single tap repainted ~15 areas (header, all three columns, the icons...) before the
 * real change moved. These setters touch LVGL only when the value differs. */
static void set_hidden(lv_obj_t *o, int hide)
{
    if (!o) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == (hide != 0)) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static void set_text(lv_obj_t *l, const char *t) { if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t); }
static void set_bg(lv_obj_t *o, uint32_t c)
{
    lv_color_t cc = lv_color_hex(c);
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), cc)) lv_obj_set_style_bg_color(o, cc, 0);
}
static void set_bgopa(lv_obj_t *o, lv_opa_t a) { if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) != a) lv_obj_set_style_bg_opa(o, a, 0); }
static void set_tc(lv_obj_t *o, uint32_t c)
{
    lv_color_t cc = lv_color_hex(c);
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), cc)) lv_obj_set_style_text_color(o, cc, 0);
}
/* Compare the configured style values, not the laid-out coordinates: before the first layout pass the
 * coordinates are stale (all zero), which would make a guard wrongly skip a move. */
static void set_xx(lv_obj_t *o, int x) { if (lv_obj_get_style_x(o, LV_PART_MAIN) != x) lv_obj_set_x(o, x); }
static void set_yy(lv_obj_t *o, int y) { if (lv_obj_get_style_y(o, LV_PART_MAIN) != y) lv_obj_set_y(o, y); }
static void set_ww(lv_obj_t *o, int w) { if (lv_obj_get_style_width(o, LV_PART_MAIN) != w) lv_obj_set_width(o, w); }
static void set_hh(lv_obj_t *o, int h) { if (lv_obj_get_style_height(o, LV_PART_MAIN) != h) lv_obj_set_height(o, h); }
static void set_pos(lv_obj_t *o, int x, int y) { if (lv_obj_get_style_x(o, LV_PART_MAIN) != x || lv_obj_get_style_y(o, LV_PART_MAIN) != y) lv_obj_set_pos(o, x, y); }

/* ------------------------------------------------------------------ widgets ---- */
#define NTG 2
static const struct { const char *label; int id; uint16_t feat; } TG[NTG] = {
    { "Conversation Awareness",       CTL_CONV_DETECT, AF_CONV_AWARE },
    { "Noise Cancellation with One AirPod", CTL_ONE_BUD_NC,  AF_ONE_BUD_NC },
};

static struct {
    /* the page */
    lv_obj_t *page, *content, *snap;     /* page scrolls; content holds the widgets; snap is its cached picture */
    lv_obj_t *title, *model;
    lv_obj_t *col[3], *dim[3], *fill[3], *pct[3], *bolt[3], *cap[3];
    lv_obj_t *img[3], *fb[3];               /* the picture, and the drawn stand-in used when a model has none */
    lv_obj_t *led;
    lv_obj_t *nc_head, *seg_box, *pill, *seg[4], *nc_caption;
    lv_obj_t *set_head, *set_box, *row[NTG], *sep[NTG], *track[NTG], *knob[NTG], *notice;
    lv_obj_t *about_head, *about_box, *about_val[3];
    /* the empty / connecting state */
    lv_obj_t *empty, *e_title, *e_body, *e_btn, *e_dim[3], *e_img[3], *e_fb[3];
} U;

static const int seg_mode[4] = { 1, 3, 4, 2 };            /* Off, Transparency, Adaptive, Noise Cancellation */
static const char *seg_name[5] = { "", "Off", "Noise Cancellation", "Transparency", "Adaptive" };

/* ---- buds and case, from flat shapes ---- */
static void art_bud(lv_obj_t *col, int right)
{
    const int w = 72;
    #define MX(x, ww) (right ? (w - (x) - (ww)) : (x))
    shape(col, MX(14, 34), 4, 34, 32, 16, 0xEDEDF2);                 /* head */
    shape(col, MX(20, 20), 10, 20, 20, 10, 0xD2D2DA);                /* ear tip */
    shape(col, MX(38, 10), 20, 10, 32, 5, 0xF4F4F8);                 /* stem */
    shape(col, MX(41, 3), 28, 3, 14, 1, 0xA9A9B1);                   /* microphone slot */
    #undef MX
}

static void art_case(lv_obj_t *col)
{
    shape(col, 6, 2, 60, 50, 16, 0xE6E6EC);
    shape(col, 14, 23, 44, 1, 0, 0xB4B4BC);                          /* lid seam */
}

/* The picture slot: a model's real render when it has one (bottom-centred), else the drawn stand-in. */
#ifndef BUD_PULL
#define BUD_PULL 0                     /* px each bud moves toward the other from the middle of its column. 12 was tried (2026-11-25) and looked wrong: 0 */
#endif
static void slot_art(lv_obj_t *img, lv_obj_t *fb, const lv_image_dsc_t *d, int dx)
{
    if (!d) { set_hidden(img, 1); set_hidden(fb, 0); return; }
    set_hidden(fb, 1); set_hidden(img, 0);
    if (lv_image_get_src(img) != d) lv_image_set_src(img, d);
    set_pos(img, (72 - (int)d->header.w) / 2 + dx, ART_H - (int)d->header.h);
}

static void make_col(int k)
{
    static const char *cap[3] = { "Left", "Right", "Case" };
    lv_obj_t *c = U.col[k] = mk(U.content, M + k * 72, HERO_Y, 72, ART_H + 74);   /* tall enough for the caption's descender (children are clipped to their parent) */
    U.fb[k] = mk(c, 0, 0, 72, ART_H);
    if (k == 2) art_case(U.fb[k]); else art_bud(U.fb[k], k);
    U.img[k] = lv_image_create(c);
    set_hidden(U.img[k], 1);
    if (k == 2) { U.led = shape(c, 34, 33, 4, 4, LV_RADIUS_CIRCLE, 0x3A3A3C); }   /* the case LED, placed by apply() */
    U.dim[k] = shape(c, 0, 0, 72, ART_H, 14, 0x000000);               /* veil: black at 60 % */
    lv_obj_set_style_bg_opa(U.dim[k], LV_OPA_60, 0);
    set_hidden(U.dim[k], 1);

    const int by = ART_H + 6;                                         /* battery row */
    lv_obj_t *box = mk(c, 18, by, 32, 14);                            /* battery outline */
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_DIM), 0);
    U.fill[k] = shape(box, 2, 2, 26, 8, 2, C_GREEN);
    shape(c, 51, by + 4, 2, 6, 1, C_DIM);                             /* nub */
    U.bolt[k] = lbl(c, 57, by - 1, 0, LV_SYMBOL_CHARGE, F_SYMBOL, C_GREEN, LV_TEXT_ALIGN_LEFT);   /* beside the battery, clear of the number */
    set_hidden(U.bolt[k], 1);

    U.pct[k] = lbl(c, 0, by + 18, 72, "", F_BIG, C_TEXT, LV_TEXT_ALIGN_CENTER);
    U.cap[k] = lbl(c, 0, by + 42, 72, cap[k], F_SMALL, C_DIM, LV_TEXT_ALIGN_CENTER);
}

/* ---- noise-control icons: anti-aliased A8 images (tools/make_icons.py), tinted white ---- */
static void icon_person(lv_obj_t *p, int mode)
{
    const lv_image_dsc_t *d = aap_icon_mode(mode);
    if (!d) return;
    lv_obj_t *a = lv_image_create(p);
    lv_image_set_src(a, d);
    lv_obj_set_style_image_recolor(a, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(a, LV_OPA_COVER, 0);
    lv_obj_center(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
}

/* ---- two small animations, the iOS way: the selector pill slides, a switch knob rolls. They only repaint
 *      a few small rectangles, so they cost next to nothing; everything else still changes at once. ---- */
static void anim_x_cb(void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, v); }

static void slide_x(lv_obj_t *o, int to, int ms)
{
    lv_anim_delete(o, anim_x_cb);
    int from = lv_obj_get_style_x(o, LV_PART_MAIN);
    if (from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static uint8_t s_knob_init[NTG], s_knob_on[NTG];
static int s_pill_on, s_pill_w;

static void knob_set(int i, int on)
{
    int tx = on ? 20 : 2;
    if (!s_knob_init[i]) { s_knob_init[i] = 1; lv_obj_set_x(U.knob[i], tx); }      /* first time: no animation */
    else if (s_knob_on[i] != on) slide_x(U.knob[i], tx, 100);
    s_knob_on[i] = (uint8_t)on;
}

/* ------------------------------------------------------------------ events ---- */
static void refresh(void) { aap_ui_force(); }

static void on_seg(lv_event_t *e);
static void on_row(lv_event_t *e);
static void on_retry(lv_event_t *e) { (void)e; if (s_ops && s_ops->connect) s_ops->connect(); }

/* optimistic selections awaiting the AirPods' echo */
static struct { uint8_t want; uint32_t t0; } PM;                         /* listening mode */
static struct { uint8_t want; uint8_t seq0; uint32_t t0; } PT[NTG];     /* toggles: 1 on, 2 off */
static uint32_t s_notice_until;
static uint8_t s_tg_shown[NTG];                                         /* what the rows show right now */

static void on_seg(lv_event_t *e)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_t || !s_t->cid) return;
    PM.want = (uint8_t)mode; PM.t0 = s_now;
    if (s_ops && s_ops->set_mode) s_ops->set_mode(mode);
    refresh();
}

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_t || !s_t->cid || i < 0 || i >= NTG) return;
    int want = s_tg_shown[i] ? 2 : 1;
    PT[i].want = (uint8_t)want; PT[i].seq0 = s_t->ctl_seq[TG[i].id]; PT[i].t0 = s_now;
    if (s_ops && s_ops->set_ctl) s_ops->set_ctl(TG[i].id, want);
    refresh();
}

/* ------------------------------------------------------------------ page cache ----
 * Scrolling used to redraw the whole page on every step (26-37 ms per frame measured on the device). The
 * page is static between taps, so it is drawn once into an RGB565 picture (aap_snap.c) while it is idle,
 * and while a drag or a fling is in progress that picture is shown instead of the live widgets: each step
 * is then one image blit. When the scroll ends the live widgets are shown again (same pixels), so taps and
 * presses always hit the real thing. If the picture is not ready (or there was no memory for it) the page
 * simply scrolls live as before. */
static lv_draw_buf_t *s_snap;
static uint8_t s_snap_ok;            /* the picture shows what the live page shows */
static uint8_t s_snap_on;            /* the picture is on screen instead of the live widgets */
static uint32_t s_snap_dirty_ms;     /* when the live page last changed */
static struct { uint32_t taken, failed, last_ms, free_big; } SN;

/* How the picture reaches the screen while scrolling:
 *   fast (a presenter was registered, the device): LVGL is told not to draw (invalidation off) and the
 *     app's post-render hook writes the picture straight to the display with aap_ui_present_tick(): a
 *     tight RGB565 -> XRGB8888 loop (LVGL's own blend of that pair is ~190 ns per pixel).
 *   image swap (no presenter, the desktop preview): the picture is an lv_image over the hidden live widgets. */
static aap_present_t s_present;
static int s_fast_y;                 /* scroll position last presented */
void aap_ui_set_present(aap_present_t f) { s_present = f; }

/* Hand the display back to LVGL: the live page is drawn again, once, in full. */
static void snap_release(void)
{
    if (!s_snap_on) return;
    s_snap_on = 0;
    if (s_present) {
        lv_display_enable_invalidation(lv_display_get_default(), true);
        lv_obj_invalidate(lv_screen_active());
    } else {
        set_hidden(U.snap, 1);
        set_hidden(U.content, 0);
    }
}

static void on_scroll(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
        if (s_snap_ok && s_snap && !s_snap_on) {
            s_snap_on = 1;
            if (s_present) {
                s_fast_y = -1;
                lv_display_enable_invalidation(lv_display_get_default(), false);
            } else {
                lv_image_set_src(U.snap, s_snap);
                set_hidden(U.snap, 0);
                set_hidden(U.content, 1);
            }
        }
    } else {
        snap_release();
    }
}

/* Called after every LVGL frame by the app: while a scroll is in progress, show the picture at the
 * current scroll position. Returns 1 if it drew. */
int aap_ui_present_tick(void)
{
    if (!s_snap_on || !s_present || !s_snap) return 0;
    int y = (int)lv_obj_get_scroll_y(U.page);
    if (y == s_fast_y) return 0;
    s_fast_y = y;
    s_present(s_snap->data, (int)s_snap->header.stride, (int)s_snap->header.h, y);
    return 1;
}

/* Called every update: once the page has been still for a moment, draw it into the picture. */
static void snap_maintain(uint32_t now, int shown)
{
    if (s_snap_on) {                                                /* safety net: SCROLL_END always arrives, but never leave LVGL muted */
        static int wd_y; static uint32_t wd_ms;
        int y = (int)lv_obj_get_scroll_y(U.page);
        lv_indev_t *in = lv_indev_get_next(NULL);
        int down = in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED;
        if (y != wd_y || down) { wd_y = y; wd_ms = now; }
        else if ((int32_t)(now - wd_ms) > 2000) snap_release();
        return;
    }
    if (s_snap_ok || !shown || SN.failed >= 3) return;
    if ((int32_t)(now - s_snap_dirty_ms) < 400) return;             /* still changing (taps, animations) */
    if (lv_anim_count_running()) return;
    lv_indev_t *in = lv_indev_get_next(NULL);
    if (in && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) return;   /* a finger is down: no highlight in the picture */

    uint32_t t0 = lv_tick_get();
    lv_result_t r = LV_RESULT_INVALID;
    if (s_snap) r = aap_snap_take(U.content, s_snap);
    if (r != LV_RESULT_OK) {                                       /* none yet, or the page grew: (re)create */
        if (s_snap) { lv_draw_buf_destroy(s_snap); s_snap = NULL; }
        s_snap = aap_snap_create(U.content);
        if (s_snap) r = aap_snap_take(U.content, s_snap);
    }
    if (r == LV_RESULT_OK) { s_snap_ok = 1; SN.taken++; }
    else { SN.failed++; if (s_snap) { lv_draw_buf_destroy(s_snap); s_snap = NULL; } }
    SN.last_ms = lv_tick_elaps(t0);
    lv_mem_monitor_t m; lv_mem_monitor(&m);
    SN.free_big = (uint32_t)m.free_biggest_size;
}

int aap_ui_snap_active(void) { return s_snap_on; }

void aap_ui_snap_stats(uint32_t *taken, uint32_t *failed, uint32_t *last_ms, uint32_t *free_big)
{
    *taken = SN.taken; *failed = SN.failed; *last_ms = SN.last_ms; *free_big = SN.free_big;
}

/* ------------------------------------------------------------------ build ---- */
void aap_ui_build(lv_obj_t *scr, int w, int h, const aap_ui_ops_t *ops)
{
    s_ops = ops; W = w; H = h; CW = w - 2 * M;
    memset(&U, 0, sizeof U);
    memset(PT, 0, sizeof PT); memset(&PM, 0, sizeof PM);
    s_snap_ok = 0; s_snap_on = 0; s_snap_dirty_ms = 0;
    memset(s_knob_init, 0, sizeof s_knob_init); memset(s_knob_on, 0, sizeof s_knob_on); s_pill_on = 0; s_pill_w = 0;
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ---- the page (scrolls) ---- */
    U.page = mk(scr, 0, 0, w, h);
    lv_obj_add_flag(U.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(U.page, LV_OBJ_FLAG_SCROLL_ELASTIC);
    U.content = mk(U.page, 0, 0, w, 700);                      /* every widget of the page; apply() sets the height */
    lv_obj_remove_flag(U.content, LV_OBJ_FLAG_CLICKABLE);
    U.snap = lv_image_create(U.page);                          /* the cached picture, shown only while scrolling */
    lv_obj_set_pos(U.snap, 0, 0);
    lv_obj_remove_flag(U.snap, LV_OBJ_FLAG_CLICKABLE);
    set_hidden(U.snap, 1);
    lv_obj_add_event_cb(U.page, on_scroll, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(U.page, on_scroll, LV_EVENT_SCROLL_END, NULL);

    U.title = lbl(U.content, M, 6, CW, "AirPods", F_BIG, C_TEXT, LV_TEXT_ALIGN_CENTER);
    U.model = lbl(U.content, M, 30, CW, "", F_SMALL, C_DIM, LV_TEXT_ALIGN_CENTER);

    for (int k = 0; k < 3; k++) make_col(k);

    /* NOISE CONTROL */
    U.nc_head = lbl(U.content, M + 14, NC_Y, CW, "NOISE CONTROL", F_SMALL, C_DIM, LV_TEXT_ALIGN_LEFT);
    U.seg_box = mk(U.content, M, NC_Y + 20, CW, 52);
    lv_obj_set_style_bg_color(U.seg_box, lv_color_hex(C_CELL), 0);
    lv_obj_set_style_bg_opa(U.seg_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.seg_box, 10, 0);
    U.pill = shape(U.seg_box, 3, 3, 10, 46, 8, C_SEG_SEL);      /* the selection; slides under the icons */
    set_hidden(U.pill, 1);
    for (int i = 0; i < 4; i++) {
        U.seg[i] = mk(U.seg_box, 3, 3, 10, 46);                 /* x / width set in apply() */
        lv_obj_set_style_radius(U.seg[i], 8, 0);
        lv_obj_set_style_bg_color(U.seg[i], lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);   /* touch feedback */
        lv_obj_set_style_bg_opa(U.seg[i], LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_add_flag(U.seg[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(U.seg[i], on_seg, LV_EVENT_CLICKED, (void *)(intptr_t)seg_mode[i]);
        icon_person(U.seg[i], seg_mode[i]);
    }
    U.nc_caption = lbl(U.content, M, NC_Y + 76, CW, "", F_BODY, C_TEXT, LV_TEXT_ALIGN_CENTER);

    /* SETTINGS: rows are positioned in apply() (a model shows only the ones it has) */
    U.set_head = lbl(U.content, M + 14, NC_Y + 106, CW, "SETTINGS", F_SMALL, C_DIM, LV_TEXT_ALIGN_LEFT);
    U.set_box = mk(U.content, M, NC_Y + 126, CW, NTG * ROW_H);
    lv_obj_set_style_bg_color(U.set_box, lv_color_hex(C_CELL), 0);
    lv_obj_set_style_bg_opa(U.set_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.set_box, 10, 0);
    for (int i = 0; i < NTG; i++) {
        U.row[i] = mk(U.set_box, 0, i * ROW_H, CW, ROW_H);
        lv_obj_set_style_radius(U.row[i], 10, 0);
        lv_obj_set_style_bg_color(U.row[i], lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);   /* touch feedback */
        lv_obj_set_style_bg_opa(U.row[i], LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_add_flag(U.row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(U.row[i], on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        para(U.row[i], 14, 10, CW - 14 - 62, TG[i].label, F_SMALL, C_TEXT, LV_TEXT_ALIGN_LEFT);
        U.track[i] = shape(U.row[i], CW - 12 - 46, (ROW_H - 28) / 2, 46, 28, 14, C_SW_OFF);
        U.knob[i] = shape(U.track[i], 2, 2, 24, 24, LV_RADIUS_CIRCLE, C_TEXT);
        U.sep[i] = shape(U.row[i], 14, ROW_H - 1, CW - 14, 1, 0, C_SEP);
    }
    U.notice = para(U.content, M + 14, 490, CW - 28, " ", F_SMALL, C_AMBER, LV_TEXT_ALIGN_LEFT);

    /* ABOUT */
    static const char *about[3] = { "Model", "Serial", "Firmware" };
    U.about_head = lbl(U.content, M + 14, 520, CW, "ABOUT", F_SMALL, C_DIM, LV_TEXT_ALIGN_LEFT);
    U.about_box = mk(U.content, M, 540, CW, 3 * 32);
    lv_obj_set_style_bg_color(U.about_box, lv_color_hex(C_CELL), 0);
    lv_obj_set_style_bg_opa(U.about_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.about_box, 10, 0);
    for (int i = 0; i < 3; i++) {
        lbl(U.about_box, 14, i * 32 + 7, 84, about[i], F_BODY, C_TEXT, LV_TEXT_ALIGN_LEFT);
        U.about_val[i] = lbl(U.about_box, 98, i * 32 + 8, CW - 98 - 14, "", F_SMALL, C_DIM, LV_TEXT_ALIGN_RIGHT);
        if (i < 2) shape(U.about_box, 14, i * 32 + 31, CW - 14, 1, 0, C_SEP);
    }

    /* ---- the empty / connecting state (no scrolling) ---- */
    U.empty = mk(scr, 0, 0, w, h);
    for (int k = 0; k < 3; k++) {
        lv_obj_t *c = mk(U.empty, M + k * 72, 70, 72, ART_H);
        U.e_fb[k] = mk(c, 0, 0, 72, ART_H);
        if (k == 2) art_case(U.e_fb[k]); else art_bud(U.e_fb[k], k);
        U.e_img[k] = lv_image_create(c);
        set_hidden(U.e_img[k], 1);
        U.e_dim[k] = shape(c, 0, 0, 72, ART_H, 14, 0x000000);
        lv_obj_set_style_bg_opa(U.e_dim[k], LV_OPA_60, 0);
    }
    U.e_title = para(U.empty, M, 160, CW, "Connect Your\nAirPods", F_BIG, C_TEXT, LV_TEXT_ALIGN_CENTER);
    U.e_body = para(U.empty, M + 8, 222, CW - 16,
                    "Pair or connect your AirPods in Settings > Bluetooth. Their battery and controls appear here.",
                    F_BODY, C_DIM, LV_TEXT_ALIGN_CENTER);
    U.e_btn = lbl(U.empty, M, 330, CW, "Try Again", F_BODY, C_BLUE, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(U.e_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(U.e_btn, 20);
    lv_obj_add_event_cb(U.e_btn, on_retry, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------------ state -> view ---- */
typedef struct {
    uint8_t state;                      /* 0 no link (empty), 1 connecting, 2 connected */
    uint16_t feat;
    char name[32], model_name[32], model_no[16], serial[16], fw[24];
    int8_t lv[3];                       /* left, right, case: -1 unknown else percent */
    uint8_t ls[3];                      /* 0 live, 1 charging, 2 last known */
    uint8_t led;                        /* case LED: 0 off, 1 green, 2 amber */
    uint8_t mode;                       /* 0 unknown else 1..4 */
    uint8_t tg[NTG];                    /* toggles shown on (after optimistic overrides) */
    uint8_t notice;                     /* an ignored change is being reported */
    uint8_t art;                        /* picture set of the model, 0 = none */
} view_t;
static view_t s_last;
static int s_have_last;

static char *put_num(char *p, int v)
{
    char b[12]; unsigned n = 0, u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) *p++ = '-';
    do { b[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *p++ = b[--n];
    return p;
}
static void copy(char *d, unsigned n, const char *s) { unsigned i = 0; for (; s && s[i] && i + 1 < n; i++) d[i] = s[i]; d[i] = 0; }

static void compute(const hb_aap_state_t *t, uint32_t now, view_t *v)
{
    memset(v, 0, sizeof *v);
    v->lv[0] = v->lv[1] = v->lv[2] = -1;
    if (!t) { copy(v->name, sizeof v->name, "AirPods"); v->feat = AF_CASE; return; }

    int connected = t->cid && t->rx_count;
    v->state = connected ? 2 : ((t->cid || t->link_avdtp) ? 1 : 0);
    const aap_model_t *m = aap_model_find(t->le_model, t->dev_model, t->dev_name);
    v->feat = m->feat;
    v->art = m->art;
    copy(v->model_name, sizeof v->model_name, m->name);
    copy(v->name, sizeof v->name, t->dev_name[0] ? t->dev_name : m->name);
    copy(v->model_no, sizeof v->model_no, t->dev_model);
    copy(v->serial, sizeof v->serial, t->dev_serial);
    copy(v->fw, sizeof v->fw, t->dev_fw);

    int fresh = t->le_seen_ms && (int32_t)(now - t->le_seen_ms) < 120000;
    int aap[3] = { t->batt_l, t->batt_r, t->batt_c };
    int ast[3] = { t->st_l, t->st_r, t->st_c };
    int le[3]  = { t->le_l, t->le_r, t->le_c };
    int lchg[3] = { (t->le_flags & LE_CHG_L) != 0, (t->le_flags & LE_CHG_R) != 0, (t->le_flags & LE_CHG_C) != 0 };
    for (int k = 0; k < 3; k++) {
        int lvl = -1, st = 0;
        int ok = (k == 2) ? (t->le_case_ms && (int32_t)(now - t->le_case_ms) < 120000) : fresh;
        if (aap[k] >= 0 && ast[k] != 4) { lvl = aap[k]; st = ast[k] == 1 ? 1 : 0; }          /* live over AAP */
        else if (le[k] >= 0 && ok) { lvl = le[k]; st = lchg[k] ? 1 : 0; }                    /* fresh over BLE */
        else if (aap[k] >= 0) { lvl = aap[k]; st = 2; }                                      /* last known */
        else if (le[k] >= 0) { lvl = le[k]; st = 2; }
        v->lv[k] = (int8_t)lvl; v->ls[k] = (uint8_t)st;
    }
    v->led = v->lv[2] < 0 ? 0 : (v->ls[2] == 1 ? 2 : (v->ls[2] == 0 ? 1 : 0));

    /* listening mode: the AirPods' word, unless our tap is still waiting for it */
    int mode = (t->mode_seen >= 1 && t->mode_seen <= 4) ? t->mode_seen : 0;
    if (PM.want) {
        if (mode == PM.want || (int32_t)(now - PM.t0) > 3000) PM.want = 0;
        else mode = PM.want;
    }
    v->mode = (uint8_t)mode;

    /* toggles: actual state, overridden by a tap until the AirPods echo it (or ignore it) */
    for (int i = 0; i < NTG; i++) {
        unsigned id = (unsigned)TG[i].id;
        int on = t->ctl_val[id] == 1;
        if (PT[i].want) {
            if (t->ctl_seq[id] != PT[i].seq0) PT[i].want = 0;                          /* echoed: trust the AirPods */
            else if ((int32_t)(now - PT[i].t0) > 2000) { PT[i].want = 0; s_notice_until = now + 4500u; }   /* ignored */
            else on = PT[i].want == 1;
        }
        v->tg[i] = (uint8_t)on;
    }
    v->notice = (int32_t)(s_notice_until - now) > 0;
}

/* ------------------------------------------------------------------ view -> widgets ---- */
static void apply(const view_t *v)
{
    uint16_t f = v->feat;
    int single = (f & AF_SINGLE) != 0, has_case = (f & AF_CASE) != 0;

    /* which state is on screen */
    set_hidden(U.page, v->state != 2);
    set_hidden(U.empty, v->state == 2);
    if (v->state != 2) {
        int connecting = v->state == 1;
        set_text(U.e_title, connecting ? "Connecting..." : "Connect Your\nAirPods");
        set_yy(U.e_body, connecting ? 196 : 222);
        set_text(U.e_body, connecting ? "Setting up the link to your AirPods."
                          : "Pair or connect your AirPods in Settings > Bluetooth. Their battery and controls appear here.");
        const aap_art_t *ea = aap_art_get(v->art ? v->art : 6);          /* unknown model: a generic pair of the current design */
        for (int k = 0; k < 3; k++) {
            set_hidden(U.e_dim[k], 0);
            slot_art(U.e_img[k], U.e_fb[k], !ea ? 0 : (k == 0 ? ea->left : (k == 1 ? ea->right : ea->cas)),
                     k == 0 ? BUD_PULL : (k == 1 ? -BUD_PULL : 0));
        }
        return;
    }

    set_text(U.title, v->name);
    set_text(U.model, v->model_name);

    /* hero: [L][R][case]; centred when the case is absent, one centred for the Max */
    set_hidden(U.col[1], single);
    set_hidden(U.col[2], single || !has_case);
    if (single)         { set_xx(U.col[0], M + 72); }
    else if (!has_case) { set_xx(U.col[0], M + 36); set_xx(U.col[1], M + 108); }
    else                { set_xx(U.col[0], M);      set_xx(U.col[1], M + 72); }
    set_text(U.cap[0], single ? "Battery" : "Left");

    /* the pictures */
    const aap_art_t *art = aap_art_get(v->art);
    /* the two buds sit close together, as Apple draws a pair; the Max (one picture) stays centred */
    slot_art(U.img[0], U.fb[0], !art ? 0 : art->left, single ? 0 : BUD_PULL);
    slot_art(U.img[1], U.fb[1], !art ? 0 : art->right, -BUD_PULL);
    slot_art(U.img[2], U.fb[2], !art ? 0 : art->cas, 0);
    if (art && art->cas && art->led_x >= 0) {                          /* the LED is painted out of the render: draw it as it is */
        set_pos(U.led, (72 - (int)art->cas->header.w) / 2 + art->led_x - 2, ART_H - (int)art->cas->header.h + art->led_y - 2);
    } else {
        set_pos(U.led, 34, 33);
    }

    for (int k = 0; k < 3; k++) {
        int lv = v->lv[k], st = v->ls[k];
        char b[8], *p = b;
        if (lv >= 0) { p = put_num(p, lv); *p++ = '%'; } else *p++ = '-';
        *p = 0;
        set_text(U.pct[k], b);
        set_tc(U.pct[k], st == 2 || lv < 0 ? C_DIM : C_TEXT);
        set_hidden(U.bolt[k], !(lv >= 0 && st == 1));
        uint32_t col = lv < 0 ? C_DIM : (lv <= 20 && st != 1 ? C_RED : C_GREEN);
        set_bg(U.fill[k], col);
        int fw = lv < 0 ? 0 : (lv * 26 + 50) / 100;
        set_ww(U.fill[k], fw < 2 ? 2 : fw);
        set_bgopa(U.fill[k], st == 2 ? LV_OPA_50 : LV_OPA_COVER);
        set_hidden(U.dim[k], lv >= 0);
    }
    set_bg(U.led, v->led == 2 ? C_AMBER : (v->led == 1 ? C_GREEN : 0x3A3A3C));
    set_hidden(U.led, art ? (!art->cas || art->led_x < 0 || v->led == 0) : 0);

    /* noise control */
    int nc = (f & AF_NC_MODES) != 0;
    set_hidden(U.nc_head, !nc); set_hidden(U.seg_box, !nc); set_hidden(U.nc_caption, !nc);
    int n = (f & AF_ADAPTIVE) ? 4 : 3, sw = (CW - 6) / n, x = 3, sel_x = -1;
    for (int i = 0; i < 4; i++) {
        if (i == 2 && !(f & AF_ADAPTIVE)) { set_hidden(U.seg[i], 1); continue; }
        set_hidden(U.seg[i], !nc);
        set_xx(U.seg[i], x);
        set_ww(U.seg[i], sw);
        if (v->mode == seg_mode[i]) sel_x = x;
        x += sw;
    }
    if (!nc || sel_x < 0) { set_hidden(U.pill, 1); s_pill_on = 0; }
    else {
        set_ww(U.pill, sw);
        if (!s_pill_on || s_pill_w != sw) { lv_anim_delete(U.pill, anim_x_cb); set_xx(U.pill, sel_x); }
        else slide_x(U.pill, sel_x, 120);
        set_hidden(U.pill, 0);
        s_pill_on = 1; s_pill_w = sw;
    }
    set_text(U.nc_caption, v->mode ? seg_name[v->mode] : " ");

    /* settings rows this model has, stacked from the top of the box */
    int y = nc ? NC_Y + 106 : NC_Y + 2, vis = 0, last = -1;
    for (int i = 0; i < NTG; i++) {
        int show = (f & TG[i].feat) != 0;
        set_hidden(U.row[i], !show);
        if (!show) continue;
        set_yy(U.row[i], vis * ROW_H);
        vis++; last = i;
        s_tg_shown[i] = v->tg[i];
        set_bg(U.track[i], v->tg[i] ? C_GREEN : C_SW_OFF);
        knob_set(i, v->tg[i]);
    }
    for (int i = 0; i < NTG; i++) set_hidden(U.sep[i], i == last);
    set_hidden(U.set_head, vis == 0); set_hidden(U.set_box, vis == 0); set_hidden(U.notice, vis == 0);
    if (vis) {
        set_yy(U.set_head, y);
        set_yy(U.set_box, y + 20);
        set_hh(U.set_box, vis * ROW_H);
        set_yy(U.notice, y + 20 + vis * ROW_H + 6);
        set_text(U.notice, v->notice ? "Could not reach the AirPods. Try again." : " ");
        y = y + 20 + vis * ROW_H + 6 + 38;                       /* the note may need two lines: reserve them */
    }

    /* about, then the scroll extent */
    set_yy(U.about_head, y + 6);
    set_yy(U.about_box, y + 26);
    set_text(U.about_val[0], v->model_no[0] ? v->model_no : "-");
    set_text(U.about_val[1], v->serial[0] ? v->serial : "-");
    set_text(U.about_val[2], v->fw[0] ? v->fw : "-");
    set_hh(U.content, y + 26 + 3 * 32 + 20);          /* the scroll extent */
}

void aap_ui_update(const hb_aap_state_t *t, uint32_t now_ms)
{
    static view_t v;
    s_t = t; s_now = now_ms;
    compute(t, now_ms, &v);
    if (!(s_have_last && memcmp(&v, &s_last, sizeof v) == 0)) {
        apply(&v);
        s_last = v;
        s_have_last = 1;
        s_snap_ok = 0;                                  /* the picture is stale until redrawn */
        s_snap_dirty_ms = now_ms;
    }
    snap_maintain(now_ms, v.state == 2);
}

/* A tap changed our pending state: repaint at once instead of waiting for the next 100 ms update. */
#ifdef AAP_UI_BENCH
/* ---- on-device render benchmark -------------------------------------------------------------
 * Each scenario sweeps the page from top to bottom in 16 steps; every step is one forced full refresh
 * (lv_refr_now: render + the runtime's flush copy), timed in microseconds. Logged as tag, avg us, max us.
 *   BFULL   everything as shipped                     BNOIMG  pictures and drawn stand-ins hidden
 *   BNOTXT  every label hidden                        BNOBOX  the three grouped boxes hidden
 *   BNONE   page hidden: a bare full-screen refresh (the floor: fill + flush)
 *   BTAP    one settings row repainted (what a toggle tap costs), no scrolling */
static struct { int on, sc, fr; uint32_t sum, max; int nh; lv_obj_t *hid[96]; } B;

static void bench_labels(lv_obj_t *o)
{
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && B.nh < 96) { B.hid[B.nh++] = c; lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); }
        } else bench_labels(c);
    }
}

static void bench_restore(void)
{
    for (int i = 0; i < B.nh; i++) lv_obj_remove_flag(B.hid[i], LV_OBJ_FLAG_HIDDEN);
    B.nh = 0;
    lv_obj_remove_flag(U.page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(U.seg_box, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(U.set_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(U.about_box, LV_OBJ_FLAG_HIDDEN);
    s_have_last = 0;                                   /* the next update repaints the live state */
}

int aap_ui_bench_step(uint32_t (*us)(void), aap_bench_log_t log)
{
    static const char *tag[6] = { "BFULL   ", "BNOIMG  ", "BNOTXT  ", "BNOBOX  ", "BNONE   ", "BTAP    " };
    if (!B.on) {
        B.on = 1; B.sc = 0; B.fr = -1;
        view_t v; memset(&v, 0, sizeof v);               /* a Pro 2 with everything known */
        v.state = 2; v.feat = NC2_BENCH; v.art = 6; v.mode = 2;
        copy(v.name, sizeof v.name, "Alex's AirPods Pro"); copy(v.model_name, sizeof v.model_name, "AirPods Pro 2 (USB-C)");
        copy(v.model_no, sizeof v.model_no, "A3048"); copy(v.serial, sizeof v.serial, "F4KE5N0AAAAA"); copy(v.fw, sizeof v.fw, "8A358");
        v.lv[0] = 83; v.lv[1] = 100; v.lv[2] = 57; v.ls[1] = 1; v.led = 1; v.tg[1] = 1;
        apply(&v);
        lv_obj_update_layout(U.page);
    }
    if (B.fr < 0) {                                       /* set the scenario up */
        bench_restore();
        switch (B.sc) {
        case 1: for (int k = 0; k < 3; k++) { set_hidden(U.img[k], 1); set_hidden(U.fb[k], 1); } break;
        case 2: bench_labels(U.page); break;
        case 3: lv_obj_add_flag(U.seg_box, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(U.set_box, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(U.about_box, LV_OBJ_FLAG_HIDDEN); break;
        case 4: lv_obj_add_flag(U.page, LV_OBJ_FLAG_HIDDEN); break;
        default: break;
        }
        lv_obj_scroll_to_y(U.page, 0, LV_ANIM_OFF);
        lv_refr_now(NULL);
        B.fr = 0; B.sum = 0; B.max = 0;
        return 1;
    }
    int32_t range = lv_obj_get_scroll_bottom(U.page) + lv_obj_get_scroll_y(U.page);
    uint32_t t0 = us();
    if (B.sc == 4)      lv_obj_invalidate(lv_screen_active());
    else if (B.sc == 5) lv_obj_invalidate(U.row[0]);
    else                lv_obj_scroll_to_y(U.page, range * B.fr / 15, LV_ANIM_OFF);
    lv_refr_now(NULL);
    uint32_t d = us() - t0;
    B.sum += d; if (d > B.max) B.max = d;
    if (++B.fr >= 16) {
        log(tag[B.sc], B.sum / 16u, B.max);
        B.fr = -1;
        if (++B.sc >= 6) { bench_restore(); lv_obj_scroll_to_y(U.page, 0, LV_ANIM_OFF); B.on = 0; return 0; }
    }
    return 1;
}
#endif

#ifdef AAP_UI_PREVIEW
void aap_ui_preview_notice(uint32_t until) { s_notice_until = until; }      /* desktop renders only */
#endif

void aap_ui_force(void)
{
    s_have_last = 0;
    aap_ui_update(s_t, s_now);
}
