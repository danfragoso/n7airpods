/* aap_snap.h — draw an object (and everything under it) into an RGB565 picture, off screen.
 *
 * This is LVGL's own snapshot routine (src/draw/snapshot/lv_snapshot.c, MIT), trimmed to what the page
 * needs and vendored here because the device's shared LVGL archive is built with LV_USE_SNAPSHOT 0, and
 * changing that shared build for one app is not worth it. */
#ifndef AAP_SNAP_H
#define AAP_SNAP_H

#include "lvgl/lvgl.h"

/* A picture buffer big enough for `obj` as it is now (w x h at RGB565). NULL: out of memory. */
lv_draw_buf_t *aap_snap_create(lv_obj_t *obj);

/* Draw `obj` into `buf` (reshaped to the object's current size; it must not be larger than the buffer
 * was created for). The object may be partly or wholly off screen. */
lv_result_t aap_snap_take(lv_obj_t *obj, lv_draw_buf_t *buf);

#endif
