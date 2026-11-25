/* aap_le.c — listen for AirPods on Bluetooth LE, and start the LE scan ourselves.
 *
 * Why: the case battery, lid state and in-case levels are not in the AAP stream (verified: the
 * AirPods report the case as "disconnected"); iOS gets them from the advertisements the pods and
 * the case broadcast (Apple company 0x004C, type 0x07, 25 bytes). The nano's LE central hands every
 * received advertisement to one handler, which btlog.c hooks (RAM 0x08438f18) and forwards here.
 *
 * Advertisement manufacturer data as the stack stores it (29 bytes, the report's raw bytes):
 *   [0..1] 4C 00 | [2] 07 | [3] 19 | [4] paired | [5..6] model (BE) | [7] status | [8] pod levels |
 *   [9] flags<<4 | case level | [10] lid | [11] colour | [12] connection | [13..28] AES-128 block
 * Layout from LibrePods (docs + BLEManager.kt); nibbles are x10 %, 0xF = unknown. The block decrypts
 * with the ENC_KEY the AirPods hand over on the AAP channel (opcode 0x30/0x31) to exact levels:
 * d[1], d[2] = pods (swapped when the right pod is primary), d[3] = case, bit7 charging, 0x7F unknown.
 *
 * Scanning: the stack only scans while someone asks it to (the heart-rate search). The same API is
 * callable from here: LeCentral::startScan(this, session, UUID vector, allowDuplicates, flag) with a
 * session record of our own (the stack only uses it as a map key and reads its name at +0x20), and
 * LeCentral::stopScan(this, session). Observed stock call: this=0x0b355e10, UUID vector of one entry,
 * dup=0, flag=1, result 0, stopped by the caller after 5.0 s.
 *
 * Variants (the app's Scan button cycles them) exist because it is not yet known whether the LE
 * module has to be powered by registering the OS app session "BTLESession" first:
 *   1: startScan only        2: register "BTLESession", wait 300 ms, startScan      3: same, wait 1.5 s
 *
 * Privacy: the ENC_KEY is held in RAM only; it is never written to the trace ring or the recorder. */
#include "hb_sdk.h"
#include "hb_aap_shared.h"
#include "compat_fixes.h"
#include "aes128.h"
#include <string.h>

#ifdef AAP_LE_HOST_TEST                 /* host tests have 64-bit pointers: skip the DRAM range check */
#define IN_DRAM(p)  ((p) != 0)
#else
#define IN_DRAM(p)  ((uint32_t)(p) >= 0x08000000u && (uint32_t)(p) < 0x0c000000u)
#endif

/* firmware 39A10023, Thumb entry points */
#define FN_LE_INSTANCE   0x083df961u   /* LeCentral *instance(void)                                  */
#define FN_LE_START      0x0843aa01u   /* uint32 startScan(this, session, uuids*, dup, flag)         */
#define FN_LE_STOP       0x0843aef9u   /* uint32 stopScan(this, session)                             */
#define FN_REG_SESSION   0x080245a5u   /* uint32 registerApplicationSession(const char*, uint32*)    */
#define SCAN_MS          5200u

typedef void *(*fn_inst_t)(void);
typedef uint32_t (*fn_start_t)(void *, void *, void *, uint32_t, uint32_t);
typedef uint32_t (*fn_stop_t)(void *, void *);
typedef uint32_t (*fn_reg_t)(const char *, uint32_t *);

static hb_aap_state_t *s_st;
static uint8_t s_key[16];
static uint8_t s_have_key;

/* A session record of our own. The stack copies a std::string from session+0x20; that string is a
 * pointer to chars with {refcount, capacity, length} in the 12 bytes before them. The refcount is huge
 * so the copies the stack makes and drops can never reach zero (and free our static storage). */
static uint8_t s_rep[32] __attribute__((aligned(8)));
static uint8_t s_sess[0x80] __attribute__((aligned(8)));
static uint32_t s_vec[3];                       /* empty UUID vector = "all UUIDs"                */

static struct {
    uint8_t busy, phase;                        /* phase 1: waiting to start, 2: scanning          */
    uint32_t t0, delay, variant;
    void *c;
} s_sc;
static uint32_t s_last_end;
static uint8_t s_reg_done;
static volatile uint8_t s_lock;

void aap_le_attach(void *state)
{
    s_st = (hb_aap_state_t *)state;
    uint32_t *w = (uint32_t *)s_rep;
    w[0] = 0x40000000u;                         /* refcount */
    w[1] = 15u;                                 /* capacity */
    w[2] = 9u;                                  /* length   */
    memcpy(s_rep + 12, "AirPodsLE", 10);
    *(uint32_t *)(s_sess + 0x20) = (uint32_t)(uintptr_t)(s_rep + 12);
    s_st->le_l = s_st->le_r = s_st->le_c = -1;
}

void aap_le_set_key(const uint8_t *key16)
{
    memcpy(s_key, key16, 16);
    s_have_key = 1;
}

/* ---------------------------------------------------------------- advertisement handling ---- */

static int nib_pct(unsigned n) { return n == 15u ? -1 : (n >= 10u ? 100 : (int)n * 10); }
static int abs_i(int v) { return v < 0 ? -v : v; }
static int is_airpods_model(unsigned m)
{
    switch (m) {
    case 0x0e20: case 0x1420: case 0x2420: case 0x0220: case 0x0f20: case 0x1320:
    case 0x1920: case 0x1b20: case 0x0a20: case 0x1f20: return 1;
    default: return (m & 0xffu) == 0x20u;       /* future models follow the same id pattern */
    }
}

/* ---- BT task side: copy only. The stack's task stack is of unknown size, so the hook does the
 * minimum (find the Apple proximity pattern, copy <= 32 bytes) and the decode happens in aap_le_poll(). */
#define QN 24u
static struct { uint8_t n; int8_t rssi; uint8_t d[32]; } s_q[QN];
static volatile uint32_t s_qw, s_qr;            /* single producer (BT task), single consumer (poll) */

/* Called in the BT task for every received advertisement (read-only: the stock handler runs after). */
void aap_le_on_adv(unsigned dev, unsigned type, const volatile unsigned char *rpt)
{
    (void)dev; (void)type;
    hb_aap_state_t *st = s_st;
    if (!st || !IN_DRAM(rpt)) return;
    st->le_raw++;

    unsigned len = (unsigned)rpt[0x14] | ((unsigned)rpt[0x15] << 8) | ((unsigned)rpt[0x16] << 16) |
                   ((unsigned)rpt[0x17] << 24);
    if (len < 21u || len > 0x400u) return;
    unsigned n = len < 32u ? len : 32u;
    int hit = 0;
    for (unsigned k = 0; k + 3u < n && !hit; k++)
        hit = rpt[0x18 + k] == 0x4c && rpt[0x19 + k] == 0x00 && rpt[0x1a + k] == 0x07 && rpt[0x1b + k] == 0x19;
    if (!hit) return;                           /* not an Apple proximity message */

    uint32_t w = s_qw;
    if (w - s_qr >= QN) return;                 /* queue full: drop (the next advertisement repeats it) */
    uint32_t slot = w % QN;
    for (unsigned k = 0; k < n; k++) s_q[slot].d[k] = rpt[0x18 + k];
    s_q[slot].n = (uint8_t)n;
    s_q[slot].rssi = (int8_t)rpt[0x12];
    __asm__ volatile("dmb sy" : : : "memory");
    s_qw = w + 1u;
}

/* ---- poll side: parse, identify, publish ---- */
static void process_adv(const uint8_t *b, unsigned n, int rssi)
{
    hb_aap_state_t *st = s_st;
    int i = -1;
    for (unsigned k = 0; k + 3u < n; k++)
        if (b[k] == 0x4c && b[k + 1] == 0x00 && b[k + 2] == 0x07 && b[k + 3] == 0x19) { i = (int)k; break; }
    if (i < 0 || (unsigned)i + 13u > n) return;

    const uint8_t *p = b + i + 2;               /* p[0] = 0x07 */
    if (!p[2]) return;                          /* paired = 0: pairing-mode broadcast, other layout */
    unsigned model = ((unsigned)p[3] << 8) | p[4];
    if (!is_airpods_model(model)) return;
    unsigned status = p[5], pods = p[6], fl = p[7], lid = p[8];

    int primary_left = (status & 0x20u) != 0, this_in_case = (status & 0x40u) != 0, flip = !primary_left;
    unsigned ln = flip ? (pods >> 4) & 15u : pods & 15u;
    unsigned rn = flip ? pods & 15u : (pods >> 4) & 15u;
    unsigned cn = fl & 15u, flags = (fl >> 4) & 15u;
    int chg_l = (flags & (flip ? 2u : 1u)) != 0, chg_r = (flags & (flip ? 1u : 2u)) != 0, chg_c = (flags & 4u) != 0;
    int x = flip ^ this_in_case;
    int ear_l = (status & (x ? 8u : 2u)) != 0, ear_r = (status & (x ? 2u : 8u)) != 0;
    int lid_open = ((lid >> 3) & 1u) == 0;

    int l = nib_pct(ln), r = nib_pct(rn), c = nib_pct(cn);
    uint32_t exact = 0;

    /* Exact levels, and the strongest identity check: only our AirPods decrypt with our key. */
    if (s_have_key && (unsigned)i + 29u <= n) {
        uint8_t d[16];
        aes128_decrypt_block(s_key, p + 11, d);
        int lv = d[flip ? 2 : 1] & 0x7f, rv = d[flip ? 1 : 2] & 0x7f, cv = d[3] & 0x7f;
        int plausible = (lv <= 100 || lv == 127) && (rv <= 100 || rv == 127) && (cv <= 100 || cv == 127) &&
                        (lv <= 100 || rv <= 100);
        int consistent = (lv > 100 || ln == 15u || abs_i(lv / 10 - (int)ln) <= 1) &&
                         (rv > 100 || rn == 15u || abs_i(rv / 10 - (int)rn) <= 1) &&
                         (cv > 100 || cn == 15u || abs_i(cv / 10 - (int)cn) <= 1);
        if (!plausible || !consistent) return;  /* decrypts to nonsense: not our AirPods */
        if (lv <= 100) l = lv;
        if (rv <= 100) r = rv;
        if (cv <= 100) c = cv;
        exact = LE_EXACT | LE_VERIFIED;
    } else {
        /* No key yet: cross-check against what the AAP channel says about the same buds, or fall
         * back to proximity when nothing else is known. */
        if (st->cid) {
            if (l >= 0 && st->batt_l >= 0 && abs_i(l - st->batt_l) > 15) return;
            if (r >= 0 && st->batt_r >= 0 && abs_i(r - st->batt_r) > 15) return;
        } else if (rssi < -75) {
            return;
        }
    }

    uint32_t now = hb_time_uptime_ms() | 1u;
    uint32_t f = exact | (chg_l ? LE_CHG_L : 0) | (chg_r ? LE_CHG_R : 0) | (chg_c ? LE_CHG_C : 0) |
                 (lid_open ? LE_LID_OPEN : 0) | (ear_l ? LE_EAR_L : 0) | (ear_r ? LE_EAR_R : 0) |
                 ((status & 0x10u) ? LE_ONE_IN_CASE : 0) | ((status & 0x04u) ? LE_BOTH_IN_CASE : 0);
    st->le_model = model;
    st->le_rssi = rssi;
    if (l >= 0) st->le_l = l;
    if (r >= 0) st->le_r = r;
    if (c >= 0) { st->le_c = c; st->le_case_ms = now; }
    st->le_flags = f;
    st->le_seen_ms = now;
    st->le_adv++;
}

/* ------------------------------------------------------------------------- scan control ---- */

static void sc_finish(uint32_t now)
{
    s_sc.busy = 0; s_sc.phase = 0;
    s_last_end = now | 1u;
    if (s_st) s_st->le_scans++;
    btl_mark("LESUM", s_st ? s_st->le_raw : 0, s_st ? s_st->le_adv : 0);
}

static void sc_start(uint32_t now)
{
    void *c = ((fn_inst_t)FN_LE_INSTANCE)();
    uint32_t running = (IN_DRAM(c) && !((uint32_t)(uintptr_t)c & 3u)) ? ((volatile uint8_t *)c)[0x10] : 0xffu;
    btl_mark("LESTART", (uint32_t)(uintptr_t)c, running);
    if (running != 1u) {                        /* BT off (disk mode) or the central is not started */
        if (s_st) s_st->le_scan_rc = -2;
        sc_finish(now);
        return;
    }
    s_sc.c = c;
    uint32_t rc = ((fn_start_t)FN_LE_START)(c, s_sess, s_vec, 0u, 1u);
    if (s_st) s_st->le_scan_rc = (int32_t)rc;
    btl_mark("LESTARTR", rc, s_sc.variant);
    if (rc != 0u) { sc_finish(now); return; }
    s_sc.phase = 2; s_sc.t0 = now;
}

static void sc_stop(uint32_t now)
{
    uint32_t rc = ((fn_stop_t)FN_LE_STOP)(s_sc.c, s_sess);
    btl_mark("LESTOPR", rc, 0);
    sc_finish(now);
}

static void sc_begin(uint32_t variant, uint32_t now)
{
    s_sc.busy = 1; s_sc.variant = variant; s_sc.t0 = now; s_sc.phase = 1; s_sc.delay = 0;
    if (s_st) s_st->le_variant = variant;
    if (variant >= 2u) {
        if (!s_reg_done) {
            uint32_t h = 0;
            uint32_t rc = ((fn_reg_t)FN_REG_SESSION)("BTLESession", &h);
            s_reg_done = (rc == 0u);
            btl_mark("LEREG", rc, h);
        }
        s_sc.delay = variant == 2u ? 300u : 1500u;
    }
}

/* Cheap when idle. Called from the transport's poll (dispatch hook and the app's tick). */
void aap_le_poll(void)
{
    hb_aap_state_t *st = s_st;
    if (!st) return;
    if (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) return;
    uint32_t now = hb_time_uptime_ms();
    while (s_qr != s_qw) {                      /* decode what the BT task queued */
        uint32_t slot = s_qr % QN;
        uint8_t b[32]; unsigned n = s_q[slot].n; int rssi = s_q[slot].rssi;
        for (unsigned k = 0; k < n && k < 32u; k++) b[k] = s_q[slot].d[k];
        __asm__ volatile("dmb sy" : : : "memory");
        s_qr = s_qr + 1u;
        process_adv(b, n, rssi);
    }
    if (s_sc.busy) {
        if (s_sc.phase == 1 && (int32_t)(now - s_sc.t0) >= (int32_t)s_sc.delay) sc_start(now);
        else if (s_sc.phase == 2 && (int32_t)(now - s_sc.t0) >= (int32_t)SCAN_MS) sc_stop(now);
    } else {
        uint32_t req = st->req_scan;
        if (!req && st->le_auto_s && (!s_last_end || (int32_t)(now - s_last_end) >= (int32_t)(st->le_auto_s * 1000u)))
            req = st->le_variant ? st->le_variant : 1u;
        if (req >= 1u && req <= 3u) { st->req_scan = 0; sc_begin(req, now); }
        else if (req) st->req_scan = 0;
    }
    s_lock = 0;
}
