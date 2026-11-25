/* Memory-only AAP safety diagnostic. No fixed-address writes, channel opens,
 * or Bluetooth sends. Requested/observed modes are NOT AirPod mode changes. */
#include "hb_sdk.h"
#include "hb_surface_input.h"
#include "hb_shared.h"
#include "hb_aap_shared.h"

static const char *MODE_NAME[4] = { "Noise Cancel", "Transparency", "Off", "Adaptive" };
static const uint8_t MODE_VAL[4] = { 2, 3, 1, 4 };
static int s_mode = -1, s_down;
static uint32_t s_taps, s_logged_seq;

static hb_aap_state_t *state(void)
{
    if (!hb_shared || hb_shared->magic != HB_SHARED_MAGIC ||
        hb_shared->version != HB_SHARED_VERSION ||
        hb_shared->aap_magic != HB_AAP_SHARED_MAGIC)
        return 0;
    hb_aap_state_t *s = (hb_aap_state_t *)hb_shared->aap_state;
    if (!s || s->magic != HB_AAP_SHARED_MAGIC || s->abi != HB_AAP_SHARED_ABI ||
        s->size != sizeof *s || s->phase != HB_AAP_SAFE_DIAGNOSTICS ||
        s->bluetooth_calls_enabled)
        return 0;
    return s;
}

static char *str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *num(char *p, uint32_t v)
{
    char b[10]; unsigned n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = b[--n];
    return p;
}
static void log_state(hb_aap_state_t *s)
{
    char b[192], *p = b;
    p = str(p, "safe-diag bt_calls=0 taps="); p = num(p, s_taps);
    p = str(p, " shared="); p = num(p, s != 0);
    if (s) {
        p = str(p, " requested="); p = num(p, s->request_mode);
        p = str(p, " seq="); p = num(p, s->request_seq);
        p = str(p, " observed="); p = num(p, s->observed_mode);
        p = str(p, " observed_seq="); p = num(p, s->observed_seq);
    }
    *p++ = '\n';
    hb_fs_mkdir("/Data/AAP");
    hb_fs_write("/Data/AAP/result.txt", b, (uint32_t)(p - b));
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    hb_trace_init();
    hb_trace_log("AAPSAFE", 1, 0);
    log_state(state());
}

void hb_raw_frame(const hb_spoint_t *t)
{
    hb_aap_state_t *s = state();
    int down = t && t->down;
    if (down && !s_down) {
        s_taps++;
        s_mode = (s_mode + 1) % 4;
        if (s) {
            s->request_mode = MODE_VAL[s_mode];
            hb_aap_barrier();
            uint32_t seq = s->request_seq + 1u;
            s->request_seq = seq ? seq : 1u;
        }
        log_state(s);
    }
    s_down = down;
    if (s && s->observed_seq != s_logged_seq) {
        s_logged_seq = s->observed_seq;
        log_state(s);
    }

    hb_draw_str(6, 6, "AAP safe diagnostics", 1, HB_WHITE, HB_BLACK);
    hb_draw_str(6, 30, "Bluetooth writes OFF", 1, HB_RGB(255,180,60), HB_BLACK);
    if (!s) {
        hb_draw_str(6, 58, "Resident update needed", 1, HB_WHITE, HB_BLACK);
        hb_draw_str(6, 82, "Power-cycle the iPod", 1, HB_WHITE, HB_BLACK);
        return;
    }
    hb_draw_str(6, 58, s_mode < 0 ? "Tap to test input" : MODE_NAME[s_mode],
                1, HB_RGB(80,255,130), HB_BLACK);
    hb_draw_str(6, 86, "Taps", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(120, 86, s_taps, 4, HB_WHITE, HB_BLACK);
    hb_draw_str(6, 110, "Observed", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(120, 110, s->observed_seq, 4, HB_WHITE, HB_BLACK);
    hb_draw_str(6, 142, "Modes are NOT sent", 1, HB_WHITE, HB_BLACK);
}
