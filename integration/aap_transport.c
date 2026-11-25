/* aap_transport — the resident owns the AAP L2CAP channel.
 *
 * Why here: FUN_083d8e60 stores the receive callback and its context in the L2CAP
 * control block, and app images are unloaded with their heap arena. Opening from
 * the app returned rc=0 and then killed the device, twice. The same call from the
 * resident's dispatch hook works and keeps working.
 *
 * Safety rules:
 *   - No writes to fixed addresses. All state lives in this file's .bss and is
 *     published through hb_shared with a magic guard.
 *   - One open per link, bounded retries, never from the touch hook.
 *   - Config argument is a >= 0x18-byte zeroed STRUCTURE (the routine reads
 *     +0x06/+0x08/+0x0c and dereferences +0x10); the 8 bytes the stack accepted
 *     from a live AVDTP entry go at the front.
 */
#include "hb_sdk.h"
#include "hb_shared.h"
#include "hb_aap_shared.h"
#include "compat_fixes.h"

/* Every transport breadcrumb also lands on the BT flight recorder's timeline (btlog.c), next to
 * what the stack itself logs, so one readout shows our action and the stack's reaction. */
#define hb_trace_log(t, a, b) do { hb_trace_log((t), (a), (b)); btl_mark((t), (a), (b)); } while (0)

#define STATE_PTR_VAR 0x080f2058u
#define CCB_SIZE      0x88u
#define MAX_CCB       24u
#define AVDTP_PSM     0x0019u
#define AAP_PSM       0x1001u

#define A_L2C_OPEN    0x083d8a61u   /* FUN_083d8e60 */

static hb_aap_state_t s_aap;
static uint8_t s_cfg[24] __attribute__((aligned(8)));
static uint8_t s_bd[6];
static uint8_t s_sinkbuf[1024] __attribute__((aligned(8)));
static int s_ready_published;
static unsigned s_settle;
static int s_bd_valid;
static unsigned s_prev_avdtp = 999u;
static uint32_t s_pending_ms;      /* open issued, channel not yet seen open */
static unsigned s_fail;            /* consecutive failed attempts */
static int s_was_open;             /* an AAP session was open */
static int s_need_new_link;        /* session ended: wait for a new AVDTP link */
static unsigned s_open_avcid;      /* AVDTP cid of the link we last opened on */
static uint32_t s_next_ms;         /* cooldown: earliest time for the next attempt */

/* open_rc sentinels for "skipped" states (negative, outside firmware range) */
#define RC_SKIP_NO_AVDTP      (-100)
#define RC_SKIP_A2DP_STREAM   (-101)
#define RC_SKIP_ONE_PER_BOOT  (-102)
#define RC_SKIP_SETTLING      (-103)
#define SETTLE_TICKS          1u

static void publish(void)
{
    if (!hb_shared) return;
    hb_shared->aap_state = &s_aap;
    __asm__ volatile("dsb sy" : : : "memory");
    hb_shared->aap_magic = HB_AAP_SHARED_MAGIC;
    s_ready_published = 1;
}

int hb_aap_poll(void);     /* forward decl for the trampoline below */

void hb_aap_init(void)
{
    s_aap.magic = HB_AAP_SHARED_MAGIC;
    s_aap.abi = HB_AAP_SHARED_ABI;
    s_aap.size = sizeof s_aap;
    s_aap.mode_seen = -1;
    s_aap.batt_l = s_aap.batt_r = s_aap.batt_c = -1;
    s_aap.ear_l = s_aap.ear_r = -1;
    s_aap.poll = &hb_aap_poll;
    s_aap.want_open = 1;   /* auto-arm: open as soon as conditions allow */
    for (unsigned i = 0; i < 64u; i++) s_aap.ctl_val[i] = s_aap.ctl_val2[i] = 0xff;
    aap_le_attach(&s_aap); /* BLE side: advertisement parser + scan control (aap_le.c) */
    if (hb_shared) publish();
}

/* Receive sink: cb(cid, data, len, fn). The buffer is shared and reused, so the
 * only safe thing to do is copy what we need now. */
static unsigned s_cb_log;
static void aap_advance(void);

/* The firmware calls this for several event kinds with different argument shapes,
 * so nothing may be dereferenced until it is proven to be a sane RAM pointer. */
__attribute__((used, noinline))
static void *aap_sink(void *a, void *b, void *c, void *d)
{
    uint32_t pb = (uint32_t)(uintptr_t)b;
    unsigned len = (unsigned)(uintptr_t)c;
    {   /* one compact line per event: the ring is circular, so the entries right
         * before a reset are what survive. */
        unsigned op = 0xffff;
        if (pb >= 0x08000000u && pb < 0x0c000000u && len >= 6 && len <= 1024u)
            op = (unsigned)((const uint8_t *)b)[4] | ((unsigned)((const uint8_t *)b)[5] << 8);
        ++s_cb_log;
        if (op == 0xffff) {
            hb_trace_log("AAPEV   ", (uint32_t)(uintptr_t)a, pb);
            hb_trace_log("AAPEV2  ", (uint32_t)(uintptr_t)c, (uint32_t)(uintptr_t)d);
        }
        hb_trace_log("AAPRX   ", ((uint32_t)len << 16) | (op & 0xffffu), hb_time_uptime_ms());
    }
    aap_advance();
    if (pb >= 0x08000000u && pb < 0x0c000000u && len >= 6 && len <= 1024u) {
        const uint8_t *p = (const uint8_t *)b;
        s_aap.rx_count++;
        unsigned op = (unsigned)p[4] | ((unsigned)p[5] << 8);
        if (op == 0x0004 && len >= 7) {                 /* battery */
            unsigned n = p[6], i = 7;
            for (unsigned k = 0; k < n && i + 4 < len; k++, i += 5) {
                int lvl = (p[i + 3] == 0x04) ? -1 : (int)p[i + 2];   /* 4 = not present */
                hb_trace_log("AAPBAT  ", ((uint32_t)p[i] << 8) | p[i + 3], (uint32_t)lvl);
                /* The whole 5-byte entry, unmapped: does a "disconnected" entry still carry a level? */
                hb_trace_log("AAPBR   ", ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) |
                                         ((uint32_t)p[i + 2] << 8) | p[i + 3], p[i + 4]);
                /* Status 4 = "disconnected" (bud in a closed case / case not seen): keep the last
                 * level we knew instead of blanking it, and publish the status so the app can mark
                 * it as stale. 1 charging, 2 discharging. */
                if (p[i] == 0x04) { s_aap.st_l = p[i + 3]; if (lvl >= 0) s_aap.batt_l = lvl; }
                else if (p[i] == 0x02) { s_aap.st_r = p[i + 3]; if (lvl >= 0) s_aap.batt_r = lvl; }
                else if (p[i] == 0x08) { s_aap.st_c = p[i + 3]; if (lvl >= 0) s_aap.batt_c = lvl; }
            }
        } else if (op == 0x002e || op == 0x0053) {      /* unknown: dump for analysis */
            /* byte-wise: the receive buffer is not word aligned */
            for (unsigned w = 0; w + 8 <= len && w < 40; w += 8) {
                uint32_t x = 0, y = 0;
                for (unsigned k = 0; k < 4; k++) {
                    x |= (uint32_t)p[w + k] << (8 * k);
                    y |= (uint32_t)p[w + 4 + k] << (8 * k);
                }
                hb_trace_log("AAPD    ", x, y);
            }
        } else if (op == 0x0006 && len >= 8) {          /* ear detection */
            s_aap.ear_l = p[6];
            s_aap.ear_r = p[7];
        } else if (op == 0x0009 && len >= 8) {            /* control notification: 09 00 id v1 v2 .. */
            unsigned id = p[6];
            if (id == 0x0d) s_aap.mode_seen = p[7];
            if (id < 64u) { s_aap.ctl_val[id] = p[7]; s_aap.ctl_val2[id] = len > 8u ? p[8] : 0u; s_aap.ctl_seq[id]++; }
        } else if (op == 0x001d && len > 14u) {          /* device information: NUL-separated strings from p[11] */
            unsigned o = 11u;
            while (o < 16u && o < len && (p[o] < 0x20 || p[o] > 0x7e)) o++;   /* tolerate a shifted start */
            char *dst[5] = { s_aap.dev_name, s_aap.dev_model, 0, s_aap.dev_serial, s_aap.dev_fw };
            unsigned cap[5] = { sizeof s_aap.dev_name, sizeof s_aap.dev_model, 0, sizeof s_aap.dev_serial, sizeof s_aap.dev_fw };
            for (unsigned f = 0; f < 5u && o < len; f++) {
                unsigned k = 0;
                for (; o < len && p[o]; o++) {
                    if (dst[f] && k + 1u < cap[f] && p[o] >= 0x20 && p[o] <= 0x7e) dst[f][k++] = (char)p[o];
                }
                if (dst[f]) dst[f][k] = 0;
                o++;                                     /* skip the NUL */
            }
            hb_trace_log("AAPINFO ", s_aap.dev_model[0], s_aap.dev_name[0]);
        } else if (op == 0x0031 && len >= 12) {          /* proximity keys: count, then {type, ?, len, ?, key} */
            unsigned cnt = p[6], o = 7;
            for (unsigned k = 0; k < cnt && o + 4u <= len; k++) {
                unsigned ty = p[o], kl = p[o + 2];
                o += 4u;
                if (o + kl > len) break;
                if (ty == 0x04 && kl == 16u) { aap_le_set_key(p + o); s_aap.key_state |= 1u; }
                else if (ty == 0x01 && kl == 16u) s_aap.key_state |= 2u;   /* IRK: noted, not stored */
                o += kl;
            }
            /* count and state only, never key bytes */
            hb_trace_log("AAPKEYS ", cnt, s_aap.key_state);
        }
        /* Raw payload (first 12 bytes after the opcode) of every other short message, so the
         * messages that arrive when a bud docks (0x0008, 0x000c, 0x000e) can be decoded offline.
         * Byte-wise: the receive buffer is not word aligned. */
        if (op != 0x0004 && op != 0x002e && op != 0x0053 && op != 0x0031 && op != 0x0030 && len <= 40u) {
            uint32_t w[3] = { 0, 0, 0 };
            for (unsigned k = 0; k < 12u && 6u + k < len; k++)
                w[k >> 2] |= (uint32_t)p[6u + k] << (8u * (k & 3u));
            hb_trace_log("AAPM    ", ((uint32_t)op << 16) | len, w[0]);
            if (len > 10u) hb_trace_log("AAPM2   ", w[1], w[2]);
        }
    }
    return s_sinkbuf;
}

static uint8_t *ccb_base(unsigned *n_out)
{
    uint8_t *st = *(uint8_t **)(uintptr_t)STATE_PTR_VAR;
    if (!st || (uint32_t)(uintptr_t)st < 0x08000000u || (uint32_t)(uintptr_t)st > 0x0c000000u)
        return 0;
    unsigned n = *(volatile uint16_t *)(st + 2);
    uint8_t *arr = *(uint8_t **)(st + 4);
    if (!arr || (uint32_t)(uintptr_t)arr < 0x08000000u || n == 0 || n > MAX_CCB)
        return 0;
    *n_out = n;
    return arr;
}

static uint8_t *find_chan(uint16_t psm, int require_open)
{
    unsigned n = 0;
    uint8_t *arr = ccb_base(&n);
    if (!arr) return 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t *c = arr + i * CCB_SIZE;
        if (*(volatile uint16_t *)(c + 4) != psm) continue;
        if (require_open && c[0] != 2) continue;
        return c;
    }
    return 0;
}

/* Count L2CAP CCBs matching psm that are fully open (state 2).
 * Used to distinguish link-idle (one AVDTP = signalling only) from
 * A2DP streaming (two AVDTP = signalling + transport). */
static unsigned count_chan_open(uint16_t psm)
{
    unsigned n = 0, hits = 0;
    uint8_t *arr = ccb_base(&n);
    if (!arr) return 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t *c = arr + i * CCB_SIZE;
        if (c[0] != 2) continue;
        if (*(volatile uint16_t *)(c + 4) != psm) continue;
        hits++;
    }
    return hits;
}

/* ---- resident-side handshake -------------------------------------------------
 * AirPods drop the whole link if the AAP channel stays silent, so the resident
 * (not the app, which cannot be running yet) must send HELLO then SUB the moment
 * the channel is open. Firmware writer entry points are the same ones the app used. */
#define A_BS_INIT   0x08410557u
#define A_BS_PUT    0x0841057fu
#define A_L2C_WRITE 0x08415ac1u
static uint8_t s_f_hello[16] = { 0,0,4,0, 1,0,2,0, 0,0,0,0, 0,0,0,0 };
static uint8_t s_f_sub[10]   = { 4,0,4,0, 0x0f,0, 0xff,0xff,0xff,0xff };
static uint8_t s_bs[64] __attribute__((aligned(8)));
static uint8_t s_f_feat[14]  = { 4,0,4,0, 0x4d,0, 0xd7,0, 0,0,0,0,0,0 };   /* LibrePods' current flags (older docs: 0xff) */
static int s_tx;                 /* frames sent so far */
static unsigned s_tx_tries;
static uint32_t s_tx_ms;
static int s_in_adv;

/* Ask for the proximity ENC_KEY (opcode 0x30, type 4): the AirPods answer with opcode 0x31. It decrypts
 * the exact battery levels in their LE advertisements and proves an advertisement is from *our* AirPods. */
static uint8_t s_f_key[8] = { 4,0,4,0, 0x30,0, 0x04,0 };

static int aap_tx(const void *pkt, int len, unsigned cid)
{
    for (unsigned i = 0; i < sizeof s_bs; i++) s_bs[i] = 0;
    if (((int (*)(void *, unsigned))A_BS_INIT)(s_bs, 2) != 0) return -1;
    int rc = ((int (*)(void *, const void *, int, unsigned))A_BS_PUT)(s_bs, pkt, len, 2);
    if (rc != 0) return rc;
    return ((int (*)(int, unsigned, unsigned, void *, int))A_L2C_WRITE)(0, cid, 0, s_bs, 0);
}

/* L2CAP write status 0x194 = the firmware has no channel object for our CID yet (the remote has not
 * accepted the connection). That is "not ready", not a failure: wait 250 ms and try again without
 * spending the retry budget. (A slow accept used to burn all retries inside the first 90 ms and the
 * handshake was never sent.) */
#define RC_CHAN_NOT_READY 0x194
static uint32_t s_tx_block_ms;
static unsigned s_early;
static void not_ready_backoff(uint32_t now)
{
    if (s_tx_tries) s_tx_tries--;
    s_tx_block_ms = now + 250u;
    if (++s_early > 80u) s_tx_tries = 10u;      /* about 20 s of "not ready": give up until the next session */
}

static void aap_advance(void)
{
    if (s_in_adv || s_tx >= 4 || s_tx_tries >= 10) return;
    uint8_t *c = find_chan(AAP_PSM, 1);
    if (!c) return;                               /* not open yet */
    unsigned cid = *(volatile uint16_t *)(c + 2);
    uint32_t now = hb_time_uptime_ms();
    if ((int32_t)(now - s_tx_block_ms) < 0) return;
    s_in_adv = 1;
    if (s_tx == 0) {
        s_tx_tries++;
        hb_trace_log("AAPTX1  ", cid, s_tx_tries);
        int rc = aap_tx(s_f_hello, sizeof s_f_hello, cid);
        hb_trace_log("AAPTX1R ", (uint32_t)rc, 0);
        if (rc == RC_CHAN_NOT_READY) not_ready_backoff(now);
        if (rc == 0) { s_tx = 1; s_tx_ms = now; }
    } else if (s_tx == 1 && (int32_t)(now - s_tx_ms) >= 150) {
        s_tx_tries++;
        hb_trace_log("AAPTX2  ", cid, s_tx_tries);
        int rc = aap_tx(s_f_sub, sizeof s_f_sub, cid);
        hb_trace_log("AAPTX2R ", (uint32_t)rc, 0);
        if (rc == RC_CHAN_NOT_READY) not_ready_backoff(now);
        if (rc == 0) { s_tx = 2; s_tx_ms = now; }
    } else if (s_tx == 2 && (int32_t)(now - s_tx_ms) >= 150) {
        s_tx_tries++;
        hb_trace_log("AAPTX3  ", cid, s_tx_tries);
        int rc = aap_tx(s_f_feat, sizeof s_f_feat, cid);
        hb_trace_log("AAPTX3R ", (uint32_t)rc, 0);
        if (rc == RC_CHAN_NOT_READY) not_ready_backoff(now);
        if (rc == 0) { s_tx = 3; s_tx_ms = now; }
    } else if (s_tx == 3 && (int32_t)(now - s_tx_ms) >= 150) {
        s_tx_tries++;
        hb_trace_log("AAPTX4  ", cid, s_tx_tries);          /* key request */
        int rc = aap_tx(s_f_key, sizeof s_f_key, cid);
        hb_trace_log("AAPTX4R ", (uint32_t)rc, 0);
        if (rc == RC_CHAN_NOT_READY) not_ready_backoff(now);
        if (rc == 0) s_tx = 4;
    }
    s_in_adv = 0;
}

/* App asks for a noise-control mode by writing req_mode (1 off, 2 anc, 3 transp, 4 adaptive);
 * only the resident touches the channel. */
static uint8_t s_f_ctl[11] = { 4,0,4,0, 9,0, 0x0a, 1, 0,0,0 };
/* App asks to change a setting: req_ctl = 0x80000000 | id << 8 | value (1 on / 2 off for the toggles). */
static void aap_send_ctl(void)
{
    uint32_t q = s_aap.req_ctl;
    if (!(q & 0x80000000u) || s_tx < 4 || s_in_adv) return;
    s_aap.req_ctl = 0;
    unsigned id = (q >> 8) & 0xffu, v = q & 0xffu;
    if (id == 0 || id >= 0x42u) return;
    uint8_t *c = find_chan(AAP_PSM, 1);
    if (!c) return;
    s_in_adv = 1;
    s_f_ctl[6] = (uint8_t)id; s_f_ctl[7] = (uint8_t)v;
    hb_trace_log("AAPCTL  ", id, v);
    int rc = aap_tx(s_f_ctl, sizeof s_f_ctl, *(volatile uint16_t *)(c + 2));
    hb_trace_log("AAPCTLR ", (uint32_t)rc, 0);
    if (rc == 0) {
        /* The AirPods apply a setting write but never echo it: they announce their settings once, at connect
         * (recorder, 2026-11-25: a write of 0x28 = 2 at t=344 s was announced back as 0x28 = 2 at the next
         * connect, and likewise for 0x28 = 1 and 0x1b both ways). So a sent write is the current value; the
         * next announcement from the AirPods overwrites it with the truth. Mode changes (0x0d) do echo. */
        if (id < 64u) { s_aap.ctl_val[id] = (uint8_t)v; s_aap.ctl_seq[id]++; }
        hb_trace_log("AAPCTLW ", id, v);
    }
    s_in_adv = 0;
}

static uint8_t s_f_mode[11] = { 4,0,4,0, 9,0, 0x0d, 2, 0,0,0 };
static void aap_send_mode(void)
{
    uint32_t m = s_aap.req_mode;
    if (!m || s_tx < 3 || s_in_adv) return;
    s_aap.req_mode = 0;
    if (m < 1 || m > 4) return;
    uint8_t *c = find_chan(AAP_PSM, 1);
    if (!c) return;
    s_in_adv = 1;
    s_f_mode[7] = (uint8_t)m;
    hb_trace_log("AAPMODE ", m, 0);
    int rc = aap_tx(s_f_mode, sizeof s_f_mode, *(volatile uint16_t *)(c + 2));
    hb_trace_log("AAPMODER", (uint32_t)rc, 0);
    s_in_adv = 0;
}

/* Called from the resident's dispatch hook. Cheap when nothing needs doing:
 * two table scans and a byte read. */
int hb_aap_poll(void)
{
    if (!s_ready_published) publish();
    aap_le_poll();                                  /* LE scan control: a few compares when idle */
    s_aap.link_avdtp = count_chan_open(AVDTP_PSM);  /* the page shows "connect your AirPods" when this is 0 */

    /* Trace every entry so we can confirm the poll is firing at all.
     * (The trace ring survives resets; see ./start trace N.) */
    s_aap.polls++;
    if ((s_aap.polls & 0x3ff) == 1)                 /* 1 in 32 to keep the ring useful */
        hb_trace_log("AAPQ    ", s_aap.polls, s_aap.want_open);

    uint8_t *aap = find_chan(AAP_PSM, 1);
    if (aap) {
        s_aap.cid = (int32_t)*(volatile uint16_t *)(aap + 2);
        s_aap.state = (aap[0x34] >= 3 && aap[0x34] <= 5) ? 2u : 1u;
        s_was_open = 1; s_fail = 0; s_pending_ms = 0;
        aap_advance();
        aap_send_mode();
        aap_send_ctl();
        return 0;
    }
    s_aap.state = 0;
    s_aap.cid = 0;
    s_tx = 0; s_tx_tries = 0; s_early = 0; s_tx_block_ms = 0;
    if (s_was_open) {                       /* session just ended */
        s_was_open = 0;
        s_need_new_link = 1;
        s_aap.mode_seen = -1;
        hb_trace_log("AAPEND  ", s_aap.opens, hb_time_uptime_ms());
    }
    if (s_aap.want_open == 2) s_pending_ms = 0;
    if (s_pending_ms) {
        if ((int32_t)(hb_time_uptime_ms() - s_pending_ms) < 8000) return 0;  /* in flight */
        s_pending_ms = 0; s_fail++;          /* never reached open */
        hb_trace_log("AAPSTUCK", s_fail, 0);
    }
    if (s_aap.link_avdtp == 0) {             /* the AirPods link is gone: whatever comes back is a NEW link, */
        s_need_new_link = 0;                 /* even if the firmware reuses the same AVDTP cid (it does)      */
        s_fail = 0;
    }
    if (s_aap.want_open == 2) {              /* the user tapped "Try again": forget every back-off */
        s_need_new_link = 0; s_fail = 0; s_next_ms = 0; s_pending_ms = 0; s_settle = 0;
        s_aap.want_open = 1;
        hb_trace_log("AAPRETRY", 0, 0);
    }
    if (s_fail >= 3) return 0;               /* give up until the link drops or the user retries */

    /* User-initiated only: the app sets want_open on the Connect tap. We
     * never fire the open unless the user has explicitly asked, so audio
     * is never disturbed unless they actually want the control channel. */
    if (!s_aap.want_open) {
        s_settle = 0;
        return 0;
    }

    /* Cache BD_ADDR and the MTU/flush words from any live AVDTP CCB.
     * l2cap_open validates cfg: [0]=MTU (>=0x80), [2]=flush (0xffff or <=0x4ff),
     * [8]=mode, which MUST be 0 or 3 (anything else returns 0x1ea). cfg+0x10 may be
     * null. So only the first 8 bytes may come from the CCB; the rest stays zero.
     * (Copying 24 bytes put CCB garbage in cfg[8] and every open failed 0x1ea.) */
    {
        uint8_t *av_tmp = find_chan(AVDTP_PSM, 1);
        if (av_tmp) {
            for (unsigned i = 0; i < sizeof s_cfg; i++) s_cfg[i] = 0;
            for (int i = 0; i < 8; i++) s_cfg[i] = av_tmp[0x40 + i];
            for (int i = 0; i < 6; i++) s_bd[i] = av_tmp[0x2e + i];
            s_bd_valid = 1;
        }
    }

    unsigned avdtp = count_chan_open(AVDTP_PSM);
    if (avdtp != s_prev_avdtp) { hb_trace_log("AAPG    ", avdtp, s_prev_avdtp); s_prev_avdtp = avdtp; }

    if (avdtp == 0) {
        /* Link is down (AirPods drop it when playback pauses): an open now only
         * leaves a dead channel record. Wait for the link to come back. */
        s_aap.open_rc = RC_SKIP_NO_AVDTP;
        s_settle = 0;
        return 0;
    }

    if (s_need_new_link) {
        uint8_t *av = find_chan(AVDTP_PSM, 1);
        unsigned avcid = av ? *(volatile uint16_t *)(av + 2) : 0;
        if (avcid == s_open_avcid) return 0;    /* same link the last session ended on */
        s_need_new_link = 0;
    }

    if (s_next_ms && (int32_t)(hb_time_uptime_ms() - s_next_ms) < 0) return 0;

    if (++s_settle < SETTLE_TICKS) {
        s_aap.open_rc = RC_SKIP_SETTLING;
        return 0;
    }
    /* cfg and BD_ADDR already populated above; cfg+0x10 is now a valid ptr */

    void *sink = (void *)((unsigned)(uintptr_t)&aap_sink | 1u);
    uint16_t cid = 0;
    s_aap.opens++;
    {
        uint8_t *av = find_chan(AVDTP_PSM, 1);
        s_open_avcid = av ? *(volatile uint16_t *)(av + 2) : 0;
    }
    s_next_ms = hb_time_uptime_ms() + 4000u;
    hb_trace_log("AAPOPEN ", AAP_PSM, 0);
    int rc = ((int (*)(void *, void *, void *, int, void *, void *, void *, void *))
              A_L2C_OPEN)(sink, sink, sink, AAP_PSM, s_bd, s_cfg, &cid, s_cfg);
    s_aap.open_rc = rc;
    s_settle = 0;
    hb_trace_log("AAPDONE ", (uint32_t)rc, cid);
    if (rc == 0) {
        s_aap.cid = (int32_t)cid;
        s_aap.state = 1;
        s_pending_ms = hb_time_uptime_ms() | 1u;
    } else {
        s_fail++;
    }
    return rc;
}
