/* btlog.c — flight recorder for the nano's Bluetooth stack (firmware 1.1.2 / 39A10023).
 *
 * Why: debugging the AirPods link by poking addresses and rebooting does not scale. Apple's BT
 * layer already narrates everything it does ("Disconnection reported for device %s", every
 * HCI/L2CAP failure, pairing, profile connect/disconnect) through ONE logger method, but that
 * method drops everything above a per-module level, and nothing reads the output. This module
 * taps that method before the filter and keeps the records in a ring we can read over USB
 * (`start peek`) while the device keeps running, so a single test session answers many questions.
 *
 * Observation only. The stock logger still runs afterwards with the original arguments, so
 * behaviour is unchanged. One aligned 32-bit store into a vtable DATA slot is the whole patch;
 * no code bytes are touched.
 *
 *   logger object: constructed by FUN_08010668 (corpus; RAM 0x08010268), vtable 0x087eaa54
 *   slot +8      : void log(this, module, file, func, line, level, fmt, ...)   RAM 0x082bff0c
 *   it compares `level` against a per-module threshold and returns early when above it.
 *
 * Opt out: create /Apps/Data/btlog.off and reboot. Every check must pass or nothing is changed.
 *
 * Record (48 bytes, little endian):
 *   w0 t_ms         hb_time_uptime_ms()
 *   w1 module<<24 | level<<16 | line
 *   w2 fmt          pointer to the format string (resolve against the firmware image on the host)
 *   w3 lr           return address in the call site (identifies the message even if fmt is odd)
 *   w4..w7          the first four variadic words, raw
 *   w8..w11         the first %s argument, copied at log time (16 bytes, NUL-terminated)
 *
 * Ring header (32 bytes) lives at the start of the block; the block address is published in the
 * boot report at BTL_REPORT_OFF. `wr` counts records ever written; slot = wr % cap.
 */
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_silver_uitask.h"
#include "compat_fixes.h"
#include <string.h>

/* LE advertisement tap. The stack's LE central calls one handler per received advertisement:
 *   FUN_08439318 (RAM 0x08438f18): void adv(LeCentral *this, LeDevice *dev, int type, Report *rpt)
 * rpt+0x12 = RSSI (signed byte), rpt+0x14 = raw data length (u32), rpt+0x18 = the raw advertising
 * bytes, inline. (Read off FUN_082d42e8, which copies exactly that into the ad-data object the
 * handler then logs as "Found device ... with ad data".) The handler only runs while the LE module
 * scans, which the stock stack does during the heart-rate-monitor search. Read-only: the original
 * handler always runs afterwards with the original arguments. */
#define BTL_LEADV_FN     0x08438f18u
#define BTL_LEADV_NSTOCK 8
static const uint8_t BTL_LEADV_STOCK[BTL_LEADV_NSTOCK] = { 0x2d,0xe9,0xff,0x4f, 0x06,0x46, 0x95,0xb0 };
static uint8_t s_leadv_tramp[32] __attribute__((aligned(8)));
static uint32_t s_leadv_seen[64];

#define BTL_SLOT_ADDR    0x087eaa5cu   /* logger vtable (0x087eaa54) + 8                      */
#define BTL_SLOT_STOCK   0x082bff0du   /* BT logger method, Thumb                             */
#define BTL_OFF_PATH     "/Apps/Data/btlog.off"
#define BTL_REPORT_OFF   0x14cu        /* boot report: ring block address (the one free word:
                                        * launch log ends at 0x14c; 0x170+ belong to disk_bootstrap) */
#define BTL_MAGIC        0x4c544254u   /* "TBTL"                                              */
#define BTL_VERSION      2u
#define BTL_REC_BYTES    48u
#define BTL_HDR_BYTES    32u
#define BTL_HEAP_LO      0x08d00000u   /* a stale slot left by an earlier boot lives in here  */
#define BTL_HEAP_HI      0x09600000u
#define BTL_CNT_SLOTS    1024u         /* per-call-site flood counters                        */

typedef struct {
    uint32_t magic;
    uint32_t wr;          /* records written since boot                                       */
    uint32_t cap;         /* record slots                                                     */
    uint32_t rec_bytes;
    uint32_t sampled_out; /* records skipped by the flood guard                               */
    uint32_t version;
    uint32_t hooked;      /* 1 once the slot points at the wrapper                            */
    uint32_t reserved;
} btl_hdr_t;

static btl_hdr_t *s_hdr;
static uint16_t   s_cnt[BTL_CNT_SLOTS];

#define BTL_IN_DRAM(p) ((uint32_t)(p) >= 0x08000000u && (uint32_t)(p) < 0x0c000000u)

/* Copy the first %s argument (device names and the like) into the record: those buffers are
 * short-lived, so resolving them later on the host would read whatever replaced them. Walks the
 * format string just far enough to find which variadic word the first %s consumes. Only pointers
 * inside DRAM are followed, so a bad value can never fault. dst is 16 bytes, always NUL-ended. */
static void btl_capture_str(uint32_t fmt, const uint32_t *args, char *dst)
{
    dst[0] = 0;
    if (!BTL_IN_DRAM(fmt)) return;
    const char *p = (const char *)fmt;
    uint32_t arg = 0;
    for (uint32_t n = 0; n < 160u && *p; n++, p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            if (*p == '*') arg++;                   /* width/precision taken from an argument */
            p++;
        }
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 't' || *p == 'j' || *p == 'L') p++;
        if (!*p || arg >= 4u) return;
        if (*p == 's') {
            uint32_t sp = args[arg];
            if (!BTL_IN_DRAM(sp)) return;
            const char *s = (const char *)sp;
            uint32_t k = 0;
            for (; k < 15u && s[k]; k++) dst[k] = (s[k] >= 32 && s[k] < 127) ? s[k] : '.';
            dst[k] = 0;
            return;
        }
        arg++;
    }
}

/* Called from the wrapper with a pointer to the saved frame:
 *   f[0..3] = r0..r3 (this, module, file, func)   f[4] = r4 (alignment filler)   f[5] = lr
 *   f[6] = line   f[7] = level   f[8] = fmt   f[9..12] = the first variadic words
 * Reading f[9..12] may run past the caller's real arguments; that is plain stack memory. */
void btl_record(const uint32_t *f)
{
    btl_hdr_t *h = s_hdr;
    if (!h) return;
    uint32_t module = f[1], lr = f[5], line = f[6], level = f[7], fmt = f[8];

    /* Flood guard: errors/warnings always pass; chatty levels keep the first 64 hits per call site
     * and then every 32nd, so a per-packet message cannot push the interesting records out. */
    if (level > 3u) {
        uint32_t k = ((lr >> 1) ^ (lr >> 11)) & (BTL_CNT_SLOTS - 1u);
        uint16_t c = s_cnt[k];
        if (c < 0xffffu) s_cnt[k] = (uint16_t)(c + 1u);
        if (c >= 64u && (c & 31u) != 0u) { h->sampled_out++; return; }
    }

    uint32_t i = __atomic_fetch_add(&h->wr, 1u, __ATOMIC_RELAXED) % h->cap;
    uint32_t *r = (uint32_t *)((uint8_t *)h + BTL_HDR_BYTES + i * BTL_REC_BYTES);
    r[0] = hb_time_uptime_ms();
    r[1] = ((module & 0xffu) << 24) | ((level & 0xffu) << 16) | (line & 0xffffu);
    r[2] = fmt;
    r[3] = lr;
    r[4] = f[9];
    r[5] = f[10];
    r[6] = f[11];
    r[7] = f[12];
    btl_capture_str(fmt, &f[9], (char *)&r[8]);     /* w8..w11: first %s argument, 16 bytes */
}

/* A mark from our own code (the AAP transport) on the same timeline as the stack's messages.
 * fmt = 0xFFFFFFFF tells the host decoder this is a mark: w8..w11 hold the tag (as passed to
 * hb_trace_log, up to 15 chars), w4/w5 the two values. Module 0xA0 distinguishes it. */
void btl_mark(const char *tag, uint32_t a, uint32_t b)
{
    btl_hdr_t *h = s_hdr;
    if (!h) return;
    uint32_t i = __atomic_fetch_add(&h->wr, 1u, __ATOMIC_RELAXED) % h->cap;
    uint32_t *r = (uint32_t *)((uint8_t *)h + BTL_HDR_BYTES + i * BTL_REC_BYTES);
    char *dst = (char *)&r[8];
    uint32_t k = 0;
    for (; tag && k < 15u && tag[k]; k++) dst[k] = tag[k];
    while (k > 0u && dst[k - 1u] == ' ') k--;       /* hb_trace_log pads tags to 8 with spaces */
    for (uint32_t z = k; z < 16u; z++) dst[z] = 0;
    r[0] = hb_time_uptime_ms();
    r[1] = (0xa0u << 24) | (5u << 16);
    r[2] = 0xffffffffu;
    r[3] = 0;
    r[4] = a;
    r[5] = b;
    r[6] = 0;
    r[7] = 0;
}

/* ---- LE advertisement tap (see the block comment at the top) ----
 * Record: w1 = 0xA1<<24 | type<<16 | len, w2 = 0xFFFFFF00 | RSSI, w3 = device object pointer
 * (stable per advertiser), w4..w11 = the first 32 raw bytes. Identical consecutive payloads from the
 * same device are skipped (scans with duplicates repeat them every few hundred ms). */
typedef void (*leadv_fn)(void *self, void *dev, int type, void *rpt);
static leadv_fn s_leadv_orig;

static void btl_raw_adv(uint32_t dev, uint32_t type, const volatile uint8_t *rpt)
{
    btl_hdr_t *h = s_hdr;
    if (!h || !BTL_IN_DRAM(rpt)) return;   /* volatile: keep every read a single byte (no merged unaligned ldr) */
    /* byte-wise reads: nothing here is guaranteed word aligned */
    uint32_t len = (uint32_t)rpt[0x14] | ((uint32_t)rpt[0x15] << 8) | ((uint32_t)rpt[0x16] << 16) |
                   ((uint32_t)rpt[0x17] << 24);
    if (len == 0u || len > 0x400u) return;
    uint32_t n = len < 32u ? len : 32u;
    const volatile uint8_t *src = rpt + 0x18;
    uint32_t sum = 0x811c9dc5u;                                 /* FNV-1a over what we keep */
    for (uint32_t k = 0; k < n; k++) sum = (sum ^ src[k]) * 16777619u;
    uint32_t slot = ((dev >> 3) ^ (dev >> 9)) & 63u;
    if (s_leadv_seen[slot] == sum) { h->sampled_out++; return; }
    s_leadv_seen[slot] = sum;

    uint32_t i = __atomic_fetch_add(&h->wr, 1u, __ATOMIC_RELAXED) % h->cap;
    uint32_t *r = (uint32_t *)((uint8_t *)h + BTL_HDR_BYTES + i * BTL_REC_BYTES);
    uint8_t *dst = (uint8_t *)&r[4];
    uint32_t k = 0;
    for (; k < n; k++) dst[k] = src[k];
    for (; k < 32u; k++) dst[k] = 0;
    r[0] = hb_time_uptime_ms();
    r[1] = (0xa1u << 24) | ((type & 0xffu) << 16) | (len & 0xffffu);
    r[2] = 0xffffff00u | rpt[0x12];
    r[3] = dev;
}

__attribute__((section(".text.hook"), used, noinline))
static void btl_leadv_hook(void *self, void *dev, int type, void *rpt)
{
    btl_raw_adv((uint32_t)(uintptr_t)dev, (uint32_t)type, (const volatile uint8_t *)rpt);
    aap_le_on_adv((unsigned)(uintptr_t)dev, (unsigned)type, (const volatile unsigned char *)rpt);   /* AirPods parser */
    s_leadv_orig(self, dev, type, rpt);        /* the stock handler, unchanged */
}

/* The entry must hold the stock bytes, or the absolute jump a previous boot left behind in DRAM
 * (ldr.w pc,[pc,#0]); the SDK helper rebuilds its trampoline from the stock bytes either way. */
static int btl_leadv_install(void)
{
    const uint8_t *p = (const uint8_t *)BTL_LEADV_FN;
    int stock = memcmp(p, BTL_LEADV_STOCK, BTL_LEADV_NSTOCK) == 0;
    int stale = p[0] == 0xdf && p[1] == 0xf8 && p[2] == 0x00 && p[3] == 0xf0;
    if (!stock && !stale) return 0;
    memset(s_leadv_seen, 0, sizeof s_leadv_seen);
    s_leadv_orig = (leadv_fn)((uintptr_t)s_leadv_tramp | 1u);   /* valid before the entry is aimed at us */
    void *t = hb_silver_patch_function_abs_stock(BTL_LEADV_FN, (void *)(uintptr_t)&btl_leadv_hook,
                                                 s_leadv_tramp, BTL_LEADV_STOCK, BTL_LEADV_NSTOCK);
    return t != 0;
}

/* ---- observation taps on the stack's LE scan API (arguments and results go to the timeline) ----
 *   LeCentral::startScan(this, session, UUIDs*, allowDuplicates, flag)  FUN_0843ae00  RAM 0x0843aa00
 *   LeCentral::stopScan(this, session)                                  (no corpus entry)  0x0843aef8
 *   registerApplicationSession(const char *name, uint32_t *handleOut)   FUN_080249a4  RAM 0x080245a4
 * Each keeps its stock 8-byte prologue in a trampoline; the originals always run, unchanged. */
typedef uint32_t (*tap_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define BTL_TAP_N 3
static const uint32_t s_tap_addr[BTL_TAP_N] = { 0x0843aa00u, 0x0843aef8u, 0x080245a4u };
static const uint8_t s_tap_stock[BTL_TAP_N][8] = {
    { 0x2d,0xe9,0xff,0x43, 0x05,0x46, 0xaf,0xb0 },     /* push.w {r0-r8,sb,lr}; mov r5,r0; sub sp,#0xbc */
    { 0x2d,0xe9,0xf3,0x41, 0x05,0x46, 0x00,0x7c },     /* push.w {r0,r1,r4-r8,lr}; mov r5,r0; ldrb r0,[r0,#16] */
    { 0x2d,0xe9,0xfe,0x43, 0x88,0x46, 0x6f,0x46 },     /* push.w {r1-r8,sb,lr}; mov r8,r1; mov r7,sp */
};
static uint8_t s_tap_tramp[BTL_TAP_N][32] __attribute__((aligned(8)));
static tap_fn s_tap_orig[BTL_TAP_N];

static inline uint32_t tap_rd(uint32_t addr)       /* aligned word read from DRAM, 0 if out of range */
{
    return (BTL_IN_DRAM(addr) && !(addr & 3u)) ? *(volatile uint32_t *)addr : 0u;
}

__attribute__((section(".text.hook"), used, noinline))
static uint32_t btl_tap_start(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    btl_mark("TSTART", a, b);                          /* this, session handle                  */
    btl_mark("TSTART2", tap_rd(c), tap_rd(c + 4u));    /* UUID vector begin, end (empty = all)   */
    btl_mark("TSTART3", d, e);                         /* allowDuplicates, 5th argument          */
    uint32_t r = s_tap_orig[0](a, b, c, d, e, f);
    btl_mark("TSTARTR", r, 0);
    return r;
}

__attribute__((section(".text.hook"), used, noinline))
static uint32_t btl_tap_stop(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    btl_mark("TSTOP", a, b);
    uint32_t r = s_tap_orig[1](a, b, c, d, e, f);
    btl_mark("TSTOPR", r, 0);
    return r;
}

__attribute__((section(".text.hook"), used, noinline))
static uint32_t btl_tap_reg(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    char nm[16];
    uint32_t k = 0;
    if (BTL_IN_DRAM(a)) for (; k < 15u && ((const volatile char *)a)[k]; k++) nm[k] = ((const volatile char *)a)[k];
    nm[k] = 0;
    uint32_t r = s_tap_orig[2](a, b, c, d, e, f);
    btl_mark(nm, r, tap_rd(b));                        /* tag = session name, status, handle out */
    return r;
}

static int btl_taps_install(void)
{
    static void *const hooks[BTL_TAP_N] = { (void *)&btl_tap_start, (void *)&btl_tap_stop, (void *)&btl_tap_reg };
    int ok = 0;
    for (int i = 0; i < BTL_TAP_N; i++) {
        const uint8_t *p = (const uint8_t *)s_tap_addr[i];
        int stock = memcmp(p, s_tap_stock[i], 8) == 0;
        int stale = p[0] == 0xdf && p[1] == 0xf8 && p[2] == 0x00 && p[3] == 0xf0;
        if (!stock && !stale) continue;
        s_tap_orig[i] = (tap_fn)((uintptr_t)s_tap_tramp[i] | 1u);
        if (hb_silver_patch_function_abs_stock(s_tap_addr[i], hooks[i], s_tap_tramp[i], s_tap_stock[i], 8))
            ok |= 1 << i;
    }
    return ok;
}

/* Installed in the logger's vtable slot. Saves the argument registers, records, restores them
 * and tail-jumps to the stock method, so the original sees exactly the call it always saw. */
__attribute__((naked, used, noinline, section(".text.hook")))
void btl_wrapper(void)
{
    __asm__ volatile(
        "push   {r0-r3, r4, lr}      \n"
        "mov    r0, sp               \n"
        "bl     btl_record           \n"
        "pop    {r0-r3, r4, lr}      \n"
        "ldr.w  r12, 1f              \n"
        "bx     r12                  \n"
        ".balign 4                   \n"
        "1: .word 0x082bff0d         \n");
}

/* 0 installed, 1 opted out, 2 slot mismatch, 3 no memory, 4 already installed */
int btlog_install(void)
{
    if (s_hdr) return 4;
    if (hb_fs_exists(BTL_OFF_PATH)) return 1;

    /* The slot must hold the stock method, or a wrapper a previous boot left behind in DRAM
     * (a Thumb pointer into the OS heap). Anything else is not the layout this was built for. */
    uint32_t cur = *(volatile uint32_t *)BTL_SLOT_ADDR;
    int stale = (cur & 1u) && cur >= BTL_HEAP_LO && cur < BTL_HEAP_HI;
    if (cur != BTL_SLOT_STOCK && !stale) return 2;

    uint32_t bytes = 128u * 1024u;
    uint8_t *raw = (uint8_t *)hb_os_alloc(bytes + 32u);
    if (!raw) { bytes = 32u * 1024u; raw = (uint8_t *)hb_os_alloc(bytes + 32u); }
    if (!raw) return 3;
    btl_hdr_t *h = (btl_hdr_t *)(((uintptr_t)raw + 31u) & ~(uintptr_t)31u);
    memset(h, 0, bytes);
    h->magic = BTL_MAGIC;
    h->rec_bytes = BTL_REC_BYTES;
    h->cap = (bytes - BTL_HDR_BYTES) / BTL_REC_BYTES;
    h->version = BTL_VERSION;
    memset(s_cnt, 0, sizeof s_cnt);
    s_hdr = h;                       /* the recorder only runs once this is set */
    __asm__ volatile("dsb sy" : : : "memory");

    *(volatile uint32_t *)BTL_SLOT_ADDR = (uint32_t)(uintptr_t)&btl_wrapper | 1u;
    __asm__ volatile("dsb sy" : : : "memory");
    h->hooked = 1;
    h->reserved = (uint32_t)btl_leadv_install();   /* bit0 = LE advertisement tap installed */
    h->reserved |= (uint32_t)btl_taps_install() << 1;   /* bit1 startScan, bit2 stopScan, bit3 registerSession taps */
    return 0;
}

/* For the boot report / trace ring: where the block is (0 if not installed). */
unsigned int btlog_block(void) { return (uint32_t)(uintptr_t)s_hdr; }
