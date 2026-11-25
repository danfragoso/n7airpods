/*
 * aap_ctl — the AirPods settings page on the iPod nano 7G (LVGL surface app).
 *
 * The page itself is aap_ui.c (pure presentation, also rendered on a desktop for design review);
 * what the AirPods can do per model is aap_models.c. This file is only the glue:
 *
 *   - The resident owns the Bluetooth side: the AAP channel (its L2CAP control block stores pointers to
 *     the receive callback, and app images are unloaded with their heap arena), the LE listener and the
 *     scan. Opening the channel from an app killed the device twice; the resident's dispatch hook is safe.
 *   - So the app only *reads* hb_aap_state_t and *records intent* in it (a tap never touches Bluetooth):
 *     req_mode, req_ctl, want_open. The resident acts on them in its poll.
 *   - A tap is not an acknowledgement: the page shows what the AirPods report back (mode_seen, ctl_val).
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_shared.h"
#include "hb_aap_shared.h"
#include "lvgl/lvgl.h"
#include "aap_ui.h"

/* ---- resident-owned transport state ---- */
static hb_aap_state_t *transport(void)
{
    if (!hb_shared || hb_shared->magic != HB_SHARED_MAGIC) return 0;
    if (hb_shared->aap_magic != HB_AAP_SHARED_MAGIC) return 0;
    hb_aap_state_t *s = (hb_aap_state_t *)hb_shared->aap_state;
    if (!s || s->magic != HB_AAP_SHARED_MAGIC || s->abi != HB_AAP_SHARED_ABI ||
        s->size != sizeof(hb_aap_state_t))
        return 0;
    return s;
}

/* ---- intents (called from touch handlers: record only) ---- */
static void op_mode(int mode)
{
    hb_aap_state_t *t = transport();
    if (t && t->cid) t->req_mode = (uint32_t)mode;
}

/* A control change (toggle): queued, then handed to the resident one at a time (it takes one request). */
static uint32_t s_ctlq[8];
static unsigned s_ctlq_r, s_ctlq_w;
static void op_ctl(int id, int value)
{
    if (s_ctlq_w - s_ctlq_r >= 8u) return;
    s_ctlq[s_ctlq_w++ % 8u] = 0x80000000u | ((uint32_t)id << 8) | (uint32_t)(value & 0xff);
}

/* "Try Again": the resident forgets every back-off and reopens the channel as soon as the link allows. */
static void op_connect(void)
{
    hb_aap_state_t *t = transport();
    if (t && !t->cid) t->want_open = 2;
}

static const aap_ui_ops_t k_ops = { op_mode, op_ctl, op_connect };

/* ---- one UI tick: drive the resident's poll, refresh the page ---- */
#define US() (*(volatile uint32_t *)0x3c700484u)       /* the free-running microsecond counter (hb_time's source) */

/* Frame probe, written to ./start trace every 2 s:
 *   UIFRM   frames, longest gap between frames (ms)
 *   UIPOLL  average, longest time in the resident's poll (us)
 *   UIREN   frames that actually drew something (render > 1.5 ms), their average render time (us)
 *   UIRENX  longest render (us)
 *   UIFLUSH time (us) spent copying dirty rectangles to the display in the window, and the pixels copied
 *   UIPRES  direct presents while scrolling: count, average us (the whole cost of a scroll frame in fast mode)
 *   UIPRESX longest present (us); ms<<16 | frames spent in fast scroll in the window (frames/ms = frame rate)
 *   UISNAP  page cache (printed when it changes): pictures<<16|failures, last ms<<24|biggest free block (bytes)
 * "render" = from the end of this callback to the post-render callback = lv_timer_handler + flush. */
static uint32_t s_t_cb_end, s_ren_n, s_ren_sum, s_ren_max;
static uint32_t s_pres_n, s_pres_sum, s_pres_max;      /* direct presents while scrolling: count, us total, longest */
static uint32_t s_fast_frames, s_fast_t0, s_fast_ms;   /* frames spent in fast scroll, and for how long (ms) */
static void aap_post(void)
{
    uint32_t d = US() - s_t_cb_end;
    if (d > 1500u) { s_ren_n++; s_ren_sum += d; if (d > s_ren_max) s_ren_max = d; }
    if (aap_ui_snap_active()) {
        uint32_t t0 = US();
        if (!s_fast_t0) s_fast_t0 = t0 | 1u;
        s_fast_frames++;
        if (aap_ui_present_tick()) {
            uint32_t pd = US() - t0;
            s_pres_n++; s_pres_sum += pd; if (pd > s_pres_max) s_pres_max = pd;
        }
        s_fast_ms += (t0 - (s_fast_t0 & ~1u)) / 1000u; s_fast_t0 = t0 | 1u;   /* time since the previous frame, summed */
    } else {
        s_fast_t0 = 0;
    }
}

#ifdef AAP_UI_BENCH
static int s_bench = 1;
static void bench_log(const char *tag, uint32_t a, uint32_t b) { hb_trace_log(tag, a, b); }
static uint32_t bench_us(void) { return US(); }
#endif

static uint32_t s_last_ui_ms;
static void aap_tick(void)
{
#ifdef AAP_UI_BENCH
    if (s_bench) {
        if (!aap_ui_bench_step(bench_us, bench_log)) { s_bench = 0; hb_trace_log("BDONE   ", 0, 0); }
        s_t_cb_end = US();
        return;
    }
#endif
    hb_aap_state_t *t = transport();
    /* Home dispatch is quiet while we are the foreground app, so the resident's poll (channel upkeep,
     * LE scan control, queued requests) is driven from here. Everything it does lives in the resident. */
    static uint32_t p_poll_sum, p_poll_max;
    uint32_t u0 = US();
    if (t && t->poll) t->poll();
    uint32_t pd = US() - u0;
    p_poll_sum += pd; if (pd > p_poll_max) p_poll_max = pd;
    if (t && t->cid && !t->req_ctl && s_ctlq_r != s_ctlq_w) t->req_ctl = s_ctlq[s_ctlq_r++ % 8u];
    uint32_t now = hb_time_uptime_ms();
    static uint32_t p_t0, p_prev, p_frames, p_gap;
    if (p_prev && now - p_prev > p_gap) p_gap = now - p_prev;
    p_prev = now; p_frames++;
    if (!p_t0) p_t0 = now;
    if (now - p_t0 >= 2000) {
        hb_trace_log("UIFRM   ", p_frames, p_gap);
        hb_trace_log("UIPOLL  ", p_frames ? p_poll_sum / p_frames : 0, p_poll_max);
        if (s_ren_n) { hb_trace_log("UIREN   ", s_ren_n, s_ren_sum / s_ren_n); hb_trace_log("UIRENX  ", s_ren_max, 0); }
        if (s_pres_n) {
            hb_trace_log("UIPRES  ", s_pres_n, s_pres_sum / s_pres_n);          /* direct presents: count, average us */
            hb_trace_log("UIPRESX ", s_pres_max, s_fast_ms << 16 | (s_fast_frames & 0xffffu));   /* longest us; ms and frames in fast scroll */
            s_pres_n = s_pres_sum = s_pres_max = 0; s_fast_frames = s_fast_ms = 0;
        }
        extern unsigned int hb_lv_flush_us, hb_lv_flush_px, hb_lv_flush_n;     /* sdk/hb_lv_surface.c */
        if (hb_lv_flush_n) { hb_trace_log("UIFLUSH ", hb_lv_flush_us, hb_lv_flush_px); hb_lv_flush_us = hb_lv_flush_px = hb_lv_flush_n = 0; }
        {
            uint32_t sn_taken, sn_failed, sn_ms, sn_free;
            aap_ui_snap_stats(&sn_taken, &sn_failed, &sn_ms, &sn_free);
            static uint32_t sn_last;
            if (sn_taken + sn_failed != sn_last) {                 /* the page cache: pictures, failures, ms, biggest free block */
                sn_last = sn_taken + sn_failed;
                hb_trace_log("UISNAP  ", (sn_taken << 16) | (sn_failed & 0xffffu), (sn_ms << 24) | (sn_free & 0xffffffu));
            }
        }
        p_t0 = now; p_frames = 0; p_gap = 0; p_poll_sum = 0; p_poll_max = 0;
        s_ren_n = 0; s_ren_sum = 0; s_ren_max = 0;
    }
    if ((int32_t)(now - s_last_ui_ms) >= 100) {           /* the page needs 10 Hz, not the frame rate */
        s_last_ui_ms = now;
        aap_ui_update(t, now);
    }
    s_t_cb_end = US();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_trace_log("AAPA", 2, 0);
    aap_ui_build(lv_screen_active(), 240, 432, &k_ops);
    aap_ui_update(transport(), hb_time_uptime_ms());
    extern unsigned int hb_lv_first_refresh_us;      /* sdk/hb_lv_surface.c: the first full-screen refresh */
    hb_trace_log("LVFIRST ", hb_lv_first_refresh_us, 0);
    {   /* the direct presenter, and touch sampling at the frame rate: LVGL reads the touch panel every 30 ms by default,
         * which caps a drag at 33 position updates a second; 8 ms follows every frame. The fling slows down per
         * read (10 % by default at 30 ms), so a shorter period needs a smaller percentage to travel as far. */
        extern void hb_lv_present565(const void *pic, int stride, int pic_h, int sy);
        aap_ui_set_present(hb_lv_present565);
        lv_indev_t *in = lv_indev_get_next(NULL);
        if (in) {
            lv_timer_t *rt = lv_indev_get_read_timer(in);
            if (rt) lv_timer_set_period(rt, 8);
            lv_indev_set_scroll_throw(in, 3);
        }
    }
    hb_lv_set_frame_cb(aap_tick);
    hb_lv_set_post_cb(aap_post);
}
