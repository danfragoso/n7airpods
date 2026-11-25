/* aap_ui.h — the AirPods page (iOS 16/17 style) for the nano's 240x432 screen.
 *
 * The page scrolls: the buds and case with their batteries, the listening mode, the settings a model has
 * (conversation awareness, one-AirPod noise cancellation) and the device info; plus an
 * Apple-style empty state when no AirPods are connected.
 *
 * Pure presentation: it reads a hb_aap_state_t and calls back through aap_ui_ops_t; it never touches
 * Bluetooth. That keeps it renderable on a desktop (tools/ui_preview) for design work. */
#ifndef AAP_UI_H
#define AAP_UI_H

#include "lvgl/lvgl.h"
#include "hb_aap_shared.h"

typedef struct {
    void (*set_mode)(int mode);            /* 1 off, 2 noise cancellation, 3 transparency, 4 adaptive */
    void (*set_ctl)(int id, int value);    /* control id, 1 = on / 2 = off                              */
    void (*connect)(void);                 /* "Try Again": force the AAP channel to be reopened         */
} aap_ui_ops_t;

void aap_ui_build(lv_obj_t *scr, int w, int h, const aap_ui_ops_t *ops);

/* Refresh from the current state. Cheap: only changed values touch LVGL. `now_ms` is the uptime. */
void aap_ui_update(const hb_aap_state_t *t, uint32_t now_ms);

#ifdef AAP_UI_BENCH
/* On-device render benchmark (debug builds only): scrolls the page through its range under several
 * "what if this were not drawn" variants and logs the cost per frame. Call once per frame until it
 * returns 0; the normal updates must be paused while it runs. `us` is a microsecond counter. */
typedef void (*aap_bench_log_t)(const char *tag, uint32_t a, uint32_t b);
int aap_ui_bench_step(uint32_t (*us)(void), aap_bench_log_t log);
#endif

/* Page-cache bookkeeping for the trace: pictures taken, failed, ms the last one took, biggest free block. */
/* The device's direct presenter (sdk hb_lv_present565): shows rows sy.. of an RGB565 picture on the display.
 * With one registered, scrolling bypasses LVGL's renderer (see aap_ui.c "page cache"). */
typedef void (*aap_present_t)(const void *pic, int stride, int pic_h, int sy);
void aap_ui_set_present(aap_present_t f);
/* Call after each LVGL frame: while a scroll is in progress, shows the picture. 1 if it drew. */
int aap_ui_present_tick(void);

/* 1 while the cached picture is on screen instead of the live widgets (tests). */
int aap_ui_snap_active(void);
void aap_ui_snap_stats(uint32_t *taken, uint32_t *failed, uint32_t *last_ms, uint32_t *free_big);

/* Repaint immediately (used after a tap, so the control moves before the AirPods answer). */
void aap_ui_force(void);

#endif
