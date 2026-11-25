/*
 * aap_open — probe: open an L2CAP channel to PSM 0x1001 (Apple Accessory Protocol)
 * on the connected AirPods, using the firmware's own channel-open routine.
 *
 * Built as a NanoApps RAW_SURFACE app (see apps/aap_open/Info.plist) so it can be
 * installed to the Home Screen instead of SCSI-exec'd: the exec path is unreliable on
 * this dock, installation is not.
 *
 * The routine is FUN_083d8e60 (image) = RAM 0x083d8a60:
 *   int open(void *cb1, void *cb2, void *cb3, int psm,
 *            void *bdaddr6, void *cfg, void *out_cid, void *ctx)
 * Guard inside: (psm & 0x101) == 1 -> 0x1001 passes (bit0 set), same as AVDTP 0x19.
 */
#include "hb_sdk.h"
#include "hb_surface_input.h"

/* NOTE the trailing ...61: the routine is Thumb code. Our app is compiled -mthumb, so a
 * call through a pointer with the low bit clear switches the CPU into ARM state at an
 * even address -> instant crash/reboot. Every Thumb function pointer needs |1. */
#define OPEN_FN       ((int (*)(void *, void *, void *, int, void *, void *, void *, void *))0x083d8a61)
/* Argument shapes copied verbatim from the SDP client's own call site
 * (FUN_080f93f0, which opens PSM 0x0001): same callbacks, same 24-byte cfg
 * layout, only the PSM differs. AVRCP's callback set made the stack reboot. */
#define CB1           0x080d27ed
#define CB2           0x080d6c89
#define CB3           0x080fe869
#define CTX           0x0877e770
/* cfg handed to the open routine: FUN_0810be18 validates it as
 *   { u16 mtu; u16 flush; ... }
 *   mtu must be >= 0x80 and <= the max from config key 0x28 (+4);
 *   flush must be 0xffff or <= 0x4ff.
 * Borrowing AVRCP's context (+0x18) failed this check and returned 0x68,
 * which I had misread as "no free channel slot". Build our own. */
static uint8_t g_cfg[24];

/* Our own inert sink. Passing another profile's callbacks (SDP's, AVRCP's) means the
 * stack later hands them an AAP event they don't expect -- that is what rebooted the
 * device a split second after the channel came up. */
static int aap_cb(void *a, void *b, void *c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    return 0;
}
#define CB_PTR ((void *)((unsigned)aap_cb | 1u))
#define AAP_PSM       0x1001

#define STATE_PTR_VAR 0x080f2058   /* holds a pointer to the channel state block */
#define CCB_SIZE      0x88
#define MAX_CCB       24

static char     g_msg[48] = "not run";
static int      g_rc = -1, g_cid = 0, g_runs = 0;
static unsigned g_chan = 0, g_open = 0, g_free = 0, g_alloc = 0, g_other = 0;
static uint8_t  g_states[MAX_CCB];
static unsigned g_last_psm = 0;
static int g_slot = -1; static unsigned g_state = 0;
static uint8_t  g_bd[8];
static int      g_was_down = 0;

#define DATA_DIR "/Data/AAP"
#define LOG_PATH "/Data/AAP/result.txt"

static char *put_dec(char *p, unsigned v, int width) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n; i < width; i++) *p++ = '0';
    while (n) *p++ = t[--n];
    return p;
}
static char *put_hex8(char *p, unsigned v) {
    const char *h = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) *p++ = h[(v >> (i * 4)) & 0xf];
    return p;
}
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static void log_result(void) {
    char b[320], *p = b;
    p = put_str(p, "rc=");      p = put_dec(p, (unsigned)g_rc, 0);
    p = put_str(p, " cid=");    p = put_dec(p, (unsigned)g_cid, 0);
    p = put_str(p, " chan=");   p = put_dec(p, g_chan, 0);
    p = put_str(p, " open=");   p = put_dec(p, g_open, 0);
    p = put_str(p, " free=");   p = put_dec(p, g_free, 0);
    p = put_str(p, " alloc3=");  p = put_dec(p, g_alloc, 0);
    p = put_str(p, " other=");  p = put_dec(p, g_other, 0);
    p = put_str(p, " runs=");   p = put_dec(p, (unsigned)g_runs, 0);
    p = put_str(p, " st=");     p = put_hex8(p, (unsigned)*(uint32_t *)STATE_PTR_VAR);
    p = put_str(p, " psm=");    p = put_hex8(p, g_last_psm);
    p = put_str(p, " bd=");
    for (int i = 0; i < 6; i++) { const char *h = "0123456789abcdef";
        *p++ = h[(g_bd[i] >> 4) & 0xf]; *p++ = h[g_bd[i] & 0xf]; }
    p = put_str(p, " slot=");   p = put_dec(p, (unsigned)(g_slot < 0 ? 99 : g_slot), 0);
    p = put_str(p, " cstate="); p = put_dec(p, g_state, 0);
    p = put_str(p, " states=");
    for (unsigned i = 0; i < g_chan && i < MAX_CCB; i++) {
        const char *h = "0123456789abcdef";
        *p++ = h[(g_states[i] >> 4) & 0xf]; *p++ = h[g_states[i] & 0xf];
    }
    p = put_str(p, " msg=");    p = put_str(p, g_msg);
    *p++ = '\n';
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    hb_fs_write(LOG_PATH, b, (uint32_t)(p - b));
}

static void setmsg(const char *s) {
    int i = 0;
    while (s[i] && i < 46) { g_msg[i] = s[i]; i++; }
    g_msg[i] = 0;
}

static int find_peer(void) {
    uint8_t *st = *(uint8_t **)STATE_PTR_VAR;
    if (!st || (unsigned)st < 0x08000000u || (unsigned)st > 0x09800000u) return 0;
    g_chan = *(volatile uint16_t *)(st + 2);
    uint8_t *arr = *(uint8_t **)(st + 4);
    if (!arr || (unsigned)arr < 0x08000000u || g_chan == 0 || g_chan > MAX_CCB) return 0;
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < g_chan; i++) {
            uint8_t *c = arr + i * CCB_SIZE;
            if (pass == 0) {
                if (i < MAX_CCB) g_states[i] = c[0];
                if (c[0] == 0) g_free++;
                else if (c[0] == 3) g_alloc++;
                else if (c[0] != 2) g_other++;
            }
            if (c[0] != 2) continue;
            if (pass == 0) g_open++;
            uint16_t psm = *(volatile uint16_t *)(c + 4);
            if (pass == 0 && psm != 0x0019 && psm != 0x0017) continue;
            g_last_psm = psm;
            int any = 0;
            for (int k = 0; k < 6; k++) { g_bd[k] = c[0x2e + k]; any |= g_bd[k]; }
            if (!any) continue;
            g_bd[6] = g_bd[7] = 0;
            return 1;
        }
    }
    return 0;
}

static void probe(void) {
    g_runs++; g_chan = g_open = 0; g_rc = -1; g_cid = 0;
    g_free = g_alloc = g_other = 0;
    for (unsigned i = 0; i < MAX_CCB; i++) g_states[i] = 0xff;
    for (unsigned i = 0; i < 24; i++) g_cfg[i] = 0;
    *(uint16_t *)(g_cfg + 0) = 0x02a0;   /* MTU 672 */
    *(uint16_t *)(g_cfg + 2) = 0x0000;   /* flush timeout (SDP passes 0) */
    *(uint16_t *)(g_cfg + 4) = 5000;     /* as SDP does */
    g_cfg[6] = 4; g_cfg[7] = 3; g_cfg[8] = 0;
    if (!find_peer()) { setmsg("no live AirPods link"); log_result(); return; }
    g_rc = OPEN_FN(CB_PTR, CB_PTR, CB_PTR, AAP_PSM,
                   g_bd, (void *)g_cfg, &g_cid, (void *)CTX);
    hb_trace_log("AOPENRC", (uint32_t)g_rc, (uint32_t)g_cid);
    if (g_rc == 0 && g_cid) {
        unsigned i;
        uint8_t *st2 = *(uint8_t **)STATE_PTR_VAR;
        uint8_t *a2 = st2 ? *(uint8_t **)(st2 + 4) : 0;
        unsigned n2 = st2 ? *(volatile uint16_t *)(st2 + 2) : 0;
        for (i = 0; a2 && i < n2 && i < MAX_CCB; i++) {
            uint8_t *c = a2 + i * CCB_SIZE;
            if (*(volatile uint16_t *)(c + 4) == AAP_PSM) { g_slot = i; g_state = c[0]; break; }
        }
        hb_trace_log("AAPOPENED", (uint32_t)g_slot, (uint32_t)g_state);
        setmsg("OPEN OK psm 0x1001");
    }
    else if (g_rc == 0)          setmsg("open returned 0, cid 0");
    else if (g_rc == 0x65)       setmsg("bad args (0x65)");
    else                         setmsg("open failed");
    log_result();
}

void hb_raw_init(int w, int h) {
    (void)w; (void)h;
    hb_trace_init();
    hb_trace_log("AAPINIT", 0, 0);
    probe();
}

void hb_raw_frame(const hb_spoint_t *t) {
    int down = t && t->down;
    if (down && !g_was_down && g_runs < 3) probe();   /* one probe per tap, max 3 */
    g_was_down = down;

    hb_draw_str(6, 8,   "nano-aap probe", 1, HB_WHITE, HB_BLACK);
    hb_draw_str(6, 40,  g_msg, 1, g_rc == 0 ? HB_GREEN : HB_RGB(0xff,0x80,0x40), HB_BLACK);
    hb_draw_str(6, 64,  "rc:", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(44, 64, (uint32_t)g_rc, 4, HB_WHITE, HB_BLACK);
    hb_draw_str(110, 64, "cid:", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(154, 64, (uint32_t)g_cid, 4, HB_WHITE, HB_BLACK);
    hb_draw_str(6, 88,  "chans:", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(66, 88, g_chan, 2, HB_WHITE, HB_BLACK);
    hb_draw_str(110, 88, "open:", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(160, 88, g_open, 2, HB_WHITE, HB_BLACK);
    hb_draw_str(6, 112, "runs:", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(66, 112, g_runs, 2, HB_WHITE, HB_BLACK);
    hb_draw_str(110, 112, "free:", 1, HB_WHITE, HB_BLACK);
    hb_draw_uint(160, 112, g_free, 2, HB_WHITE, HB_BLACK);
    for (int i = 0; i < 6; i++) hb_draw_hex16((int16_t)(6 + 24*i), 136, g_bd[i], HB_WHITE, HB_BLACK);
    hb_draw_str(6, HB_SCREEN_H - 18, "tap to re-run", 1, HB_RGB(0x80,0x80,0x80), HB_BLACK);
}
