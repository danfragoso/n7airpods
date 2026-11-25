/* aap_snap.c — see aap_snap.h. Adapted from LVGL's lv_snapshot.c (MIT licence, LVGL Kft). */
#include "aap_snap.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"

lv_draw_buf_t *aap_snap_create(lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    int32_t w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0) return NULL;
    return lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
}

lv_result_t aap_snap_take(lv_obj_t *obj, lv_draw_buf_t *draw_buf)
{
    if (!obj || !draw_buf) return LV_RESULT_INVALID;

    lv_obj_update_layout(obj);
    int32_t w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    if (w <= 0 || h <= 0) return LV_RESULT_INVALID;
    if (!lv_draw_buf_reshape(draw_buf, LV_COLOR_FORMAT_UNKNOWN, (uint32_t)w, (uint32_t)h, LV_STRIDE_AUTO))
        return LV_RESULT_INVALID;

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_draw_buf_clear(draw_buf, NULL);                       /* black: the page background */

    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = draw_buf;
    layer.buf_area.x1 = area.x1;
    layer.buf_area.y1 = area.y1;
    layer.buf_area.x2 = area.x1 + w - 1;
    layer.buf_area.y2 = area.y1 + h - 1;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = area;
    layer.phy_clip_area = area;

    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);

    lv_display_t *disp_old = lv_refr_get_disp_refreshing();
    lv_display_t *disp_new = lv_obj_get_display(obj);
    lv_layer_t *layer_old = disp_new->layer_head;
    disp_new->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp_new);

    lv_obj_redraw(&layer, obj);

    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    disp_new->layer_head = layer_old;
    lv_refr_set_disp_refreshing(disp_old);

    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
    return LV_RESULT_OK;
}
