/* AAP transport state shared between the resident and the app.
 *
 * The resident must own the AAP channel: the L2CAP control block stores pointers
 * to the receive callback and its buffers, and app images are unloaded with their
 * heap arena. Opening from the app killed the device twice (see docs/FINDINGS.md),
 * while the same call from the resident's dispatch hook works.
 *
 * The app therefore only *reads* this block (for display) and never dereferences
 * anything it does not own.
 */
#ifndef HB_AAP_SHARED_H
#define HB_AAP_SHARED_H

#include <stdint.h>

#define HB_AAP_SHARED_MAGIC 0x32504141u   /* 'AAP2' */
#define HB_AAP_SHARED_ABI   11u

typedef struct {
    uint32_t magic;              /* HB_AAP_SHARED_MAGIC                          */
    uint32_t abi;                /* HB_AAP_SHARED_ABI                            */
    uint32_t size;               /* sizeof(hb_aap_state_t)                       */
    volatile uint32_t state;     /* 0 idle, 1 channel open, 2 writable           */
    volatile uint32_t want_open; /* 1 = the app asked for the channel            */
    volatile int32_t  cid;       /* AAP local cid, 0 when none                   */
    volatile int32_t  open_rc;   /* last open result (incl. negative sentinels)  */
    volatile uint32_t opens;     /* open attempts this boot                      */
    volatile uint32_t polls;     /* hb_aap_poll entries this boot (liveness)     */
    volatile uint32_t rx_count;  /* notification packets received                */
    volatile int32_t  mode_seen; /* from 09 00 0D, -1 unknown                    */
    volatile int32_t  batt_l;    /* -1 unknown                                   */
    volatile int32_t  batt_r;
    volatile int32_t  batt_c;
    volatile int32_t  ear_l;     /* 0 in ear, 1 out, 2 case, -1 unknown          */
    volatile int32_t  ear_r;
    /* Resident-owned trampoline: lets the app drive the poll from its own
     * LVGL tick while in-app, since Home dispatch is quiet then. All state
     * reached through this pointer lives in resident .bss. */
    int (*poll)(void);
    /* App -> resident: requested noise mode (1 off, 2 anc, 3 transparency,
     * 4 adaptive). The resident sends it and clears the field. */
    volatile uint32_t req_mode;
    /* Battery entry status as reported (1 charging, 2 discharging, 4 disconnected, 0 unknown).
     * With status 4 the level above is the last known one, not a live reading. */
    volatile int32_t  st_l;
    volatile int32_t  st_r;
    volatile int32_t  st_c;

    /* ---- Bluetooth LE advertisement view (ABI 9), published by the resident ----
     * The AirPods and their case broadcast battery, lid and in-ear state; the resident listens
     * during short LE scans. Levels are 10 % steps, or exact when LE_EXACT is set (the encrypted
     * part of the advertisement decoded with the key the AirPods hand over on the AAP channel).
     * The key itself never leaves the resident. */
    volatile uint32_t le_model;  /* BLE model id: 0x0E20 Pro, 0x1420 Pro 2, 0x2420 Pro 2 USB-C, ... 0 = none */
    volatile int32_t  le_l;      /* percent, -1 unknown                                  */
    volatile int32_t  le_r;
    volatile int32_t  le_c;      /* last valid case level (see le_case_ms for its age)   */
    volatile uint32_t le_flags;  /* LE_* bits below                                      */
    volatile int32_t  le_rssi;   /* dBm of the last accepted advertisement               */
    volatile uint32_t le_seen_ms;/* uptime of the last accepted advertisement, 0 = never */
    volatile uint32_t le_case_ms;/* uptime when le_c was last valid, 0 = never           */
    volatile uint32_t le_adv;    /* AirPods advertisements accepted                      */
    volatile uint32_t le_raw;    /* advertisements of any device the LE handler has seen */
    volatile uint32_t le_scans;  /* scans completed                                      */
    volatile int32_t  le_scan_rc;/* last startScan result; negative = refused before the call */
    volatile uint32_t le_variant;/* scan variant last used                               */
    volatile uint32_t req_scan;  /* app -> resident: 0 none, 1..3 = run one scan (variant) */
    volatile uint32_t le_auto_s; /* app -> resident: rescan every N s while polled, 0 = off  */
    volatile uint32_t key_state; /* bit0: ENC_KEY received, bit1: IRK received           */

    /* ---- device information and control state (ABI 10) ---- */
    char dev_name[32];           /* from the AirPods' device-information message (opcode 0x1D) */
    char dev_model[16];          /* model number, e.g. "A3048"                           */
    char dev_serial[16];
    char dev_fw[24];
    volatile uint8_t ctl_val[64];   /* last value byte per control id (09 00 id v ...), 0xFF = not seen */
    volatile uint8_t ctl_val2[64];  /* second value byte (press-and-hold sends one per bud)           */
    volatile uint32_t req_ctl;      /* app -> resident: 0x80000000 | id << 8 | value; sent, then cleared */

    /* ---- link visibility and echoes (ABI 11) ---- */
    volatile uint32_t link_avdtp;   /* open AVDTP channels right now: 0 = no AirPods link, else A2DP is up */
    volatile uint8_t  ctl_seq[64];  /* counts control notifications per id: an echo is detectable even when
                                       the value did not change (a setting the AirPods ignore never echoes) */
} hb_aap_state_t;

/* control ids (09 00 id value ...) */
#define CTL_EAR_DETECT   0x0au   /* 1 on, 2 off              */
#define CTL_CLICK_HOLD   0x16u   /* val = right bud, val2 = left bud; 1 noise control, 5 Siri */
#define CTL_LISTEN_CFG   0x1au   /* bitmask of modes in the rotation */
#define CTL_ONE_BUD_NC   0x1bu   /* 1 on, 2 off              */
#define CTL_CONV_DETECT  0x28u   /* 1 on, 2 off              */
#define CTL_IN_CASE_TONE 0x31u

#define LE_CHG_L        (1u << 0)   /* left bud charging                */
#define LE_CHG_R        (1u << 1)
#define LE_CHG_C        (1u << 2)   /* case charging                    */
#define LE_LID_OPEN     (1u << 3)
#define LE_EAR_L        (1u << 4)   /* left bud in ear                  */
#define LE_EAR_R        (1u << 5)
#define LE_ONE_IN_CASE  (1u << 6)
#define LE_BOTH_IN_CASE (1u << 7)
#define LE_EXACT        (1u << 8)   /* levels are exact (decrypted)     */
#define LE_VERIFIED     (1u << 9)   /* identity confirmed by the key    */

#endif /* HB_AAP_SHARED_H */
