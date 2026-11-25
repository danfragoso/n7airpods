/*
 * did_spoof.c — present the nano to AirPods with Apple's Bluetooth-SIG vendor id (LibrePods "VendorID spoofing").
 *
 * What the stack does at start-up (BT::StackManager::startupStack, corpus FUN_082be... near "Starting
 * bluetooth core stack"): it fills a 0x1c-byte Device-ID record and registers it with the SDP server,
 *
 *     OI_DI_Register(record, &handle)     FUN_080f38c0   RAM 0x080f34c0
 *       u16 +0x00  vendor id        0x05AC  (FUN_082b7860: "return 0x5ac")
 *       u16 +0x02  product id       FUN_08417e3a()
 *       u16 +0x04  version          FUN_082bf97e()
 *       u32 +0x08  primary record   1
 *       u16 +0x0c  vendor id source 2  (USB Implementers Forum)
 *
 * and OI_DI_Register copies the fields into SDP attributes 0x0201/0x0202/0x0203/0x0204/0x0205 at once.
 * AirPods read this record. LibrePods found that vendor 0x004C with source 1 (Bluetooth SIG) makes them
 * treat the host as an Apple device and unlock extra features (their change: BTA_DmSetLocalDiRecord,
 * vendor = 0x004C, vendor_id_source = 0x0001, nothing else). This does the same one step earlier: it
 * edits those two fields in the caller's record and then runs the stock function unchanged.
 *
 * Every boot's stack start passes through here (the stack restarts whenever USB mode toggles), so it takes
 * effect on the next plug/unplug cycle. The remote may cache what it read when it paired; if so, forget
 * and re-pair the AirPods once.
 *
 * RESULT (2026-11-25): with the spoof active the AirPods announced exactly the same settings at connect
 * and the link behaved the same, so it unlocks nothing the page uses. The toggles that "did not work" were
 * working all along (see aap_send_ctl). OFF BY DEFAULT now; it only logs the record.
 *
 * Opt in: create /Apps/Data/did.on and reboot (the record is then changed).
 * Marks on the flight recorder: DIREG (as the stack built it: vendor<<16|source, product<<16|version),
 * DIREG2 (what was registered).
 */
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_silver_uitask.h"
#include "compat_fixes.h"
#include <string.h>

#define DI_FN         0x080f34c0u
#define DI_NSTOCK     8
static const uint8_t DI_STOCK[DI_NSTOCK] = { 0x2d,0xe9,0xf0,0x4f, 0x4f,0xf0,0x07,0x07 };   /* push.w {r4-fp,lr}; mov.w r7,#7 */
#define DI_ON_PATH    "/Apps/Data/did.on"
#define DI_IN_DRAM(p) ((uint32_t)(p) >= 0x08000000u && (uint32_t)(p) < 0x0c000000u)

#define APPLE_VENDOR  0x004Cu
#define SRC_BT_SIG    0x0001u

typedef int (*di_fn)(volatile uint16_t *rec, void *handle);
static uint8_t s_di_tramp[32] __attribute__((aligned(8)));
static di_fn s_di_orig;
static volatile uint32_t s_di_spoof;       /* 1 = change the record */
static volatile uint32_t s_di_calls;

__attribute__((section(".text.hook"), used, noinline))
static int di_hook(volatile uint16_t *rec, void *handle)
{
    s_di_calls++;
    if (DI_IN_DRAM(rec) && !((uintptr_t)rec & 1u) && rec[6] >= 1u && rec[6] <= 2u) {
        btl_mark("DIREG", ((uint32_t)rec[0] << 16) | rec[6], ((uint32_t)rec[1] << 16) | rec[2]);
        if (s_di_spoof) {
            rec[0] = (uint16_t)APPLE_VENDOR;
            rec[6] = (uint16_t)SRC_BT_SIG;
        }
        btl_mark("DIREG2", ((uint32_t)rec[0] << 16) | rec[6], ((uint32_t)rec[1] << 16) | rec[2]);
    }
    return s_di_orig(rec, handle);
}

/* 0 = patched and spoofing, 1 = stock bytes did not match, 2 = logging only (default) */
int did_spoof_install(void)
{
    const uint8_t *p = (const uint8_t *)DI_FN;
    int stock = memcmp(p, DI_STOCK, DI_NSTOCK) == 0;
    int stale = p[0] == 0xdf && p[1] == 0xf8 && p[2] == 0x00 && p[3] == 0xf0;   /* absolute jump left by an earlier boot */
    if (!stock && !stale) return 1;
    int on = hb_fs_exists(DI_ON_PATH);
    s_di_spoof = on ? 1u : 0u;
    s_di_orig = (di_fn)((uintptr_t)s_di_tramp | 1u);
    void *t = hb_silver_patch_function_abs_stock(DI_FN, (void *)(uintptr_t)&di_hook, s_di_tramp, DI_STOCK, DI_NSTOCK);
    if (!t) return 1;
    return on ? 0 : 2;
}

unsigned did_spoof_calls(void) { return s_di_calls; }
