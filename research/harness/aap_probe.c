/*
 * aap_probe — Phase 1: prove we can open L2CAP PSM 0x1001 and talk AAP.
 *
 * Runs as a NanoApps harness (`./start run aap_probe`), i.e. SCSI-exec'd once,
 * RAM only, nothing written to the nano. Draws its state directly to the LCD and
 * logs to the resident trace ring so `./start trace` shows the bytes the AirPods
 * send back.
 *
 * The transport is deliberately behind two functions (see transport_stub.c).
 * Until G0/G1 tell us which BLUEmagic entry point to call, the stub returns
 * -ENOSYS and this program is a plumbing test: it proves the harness runs, the
 * packets are built correctly, and the trace ring captures them.
 *
 * Modelled on NanoApps harnesses/fs_test (raw MIPI drawing, no libc).
 */
#include "hb_sdk.h"
#include "aap_packets.h"

/* ---------------------------------------------------------------------------
 * Transport contract. Implemented in transport_stub.c today; Phase 1 replaces
 * the bodies with real BLUEmagic calls once tools/survey.sh names them.
 *
 *   open  : open an L2CAP channel to the connected accessory on PSM 0x1001
 *   write : send one AAP frame
 *   read  : non-blocking poll for a received AAP frame, 0 if none pending
 * ------------------------------------------------------------------------ */
int  nano_aap_link_open(void);
int  nano_aap_link_write(const uint8_t *buf, unsigned len);
int  nano_aap_link_read(uint8_t *buf, unsigned max, unsigned *got);

/* ---- drawing helpers ---------------------------------------------------- */
static void line(int y, const char *s) {
    hb_draw_str(6, (int16_t)y, s, 1, HB_WHITE, HB_BLACK);
}

static const char *mode_name(int m) {
    switch (m) {
        case AAP_MODE_OFF:          return "OFF";
        case AAP_MODE_ANC:          return "ANC";
        case AAP_MODE_TRANSPARENCY: return "TRANSPARENCY";
        case AAP_MODE_ADAPTIVE:     return "ADAPTIVE";
        default:                    return "-";
    }
}

static void draw_num(int x, int y, unsigned v, hb_color_t fg) {
    char tmp[4]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < t; i++)
        hb_draw_char((int16_t)(x + 8 * i), (int16_t)y, tmp[t - 1 - i], 1, fg, HB_BLACK);
}

static void render(int link, int stage, int mode, const aap_battery_t *bat,
                   const aap_ears_t *ears, uint32_t rx_count, const uint8_t *last, unsigned last_len) {
    line(8,  "nano-aap / Phase 1 probe");
    line(30, link ? "L2CAP 0x1001: OPEN" : "L2CAP 0x1001: not open");
    line(42, stage == 0 ? "stage: idle" :
             stage == 1 ? "stage: handshake sent" :
             stage == 2 ? "stage: subscribed" : "stage: streaming");

    line(64, "mode:");
    hb_draw_str(52, 64, mode_name(mode), 1, HB_GREEN, HB_BLACK);

    line(86, "battery L");
    line(98, "        R");
    if (bat->have_left)  draw_num(88, 86, bat->left,  HB_WHITE);
    if (bat->have_right) draw_num(88, 98, bat->right, HB_WHITE);
    if (!bat->have_left && !bat->have_right)
        hb_draw_str(88, 86, "--", 1, HB_RGB(0x80, 0x80, 0x80), HB_BLACK);

    line(120, "ears:");
    hb_draw_str(52, 120,
                ears->primary == 0 ? "P:in" : ears->primary == 1 ? "P:out" :
                ears->primary == 2 ? "P:case" : "P:?",
                1, HB_WHITE, HB_BLACK);

    line(142, "rx packets:");
    hb_draw_uint(100, 142, rx_count, 4, HB_WHITE, HB_BLACK);

    line(164, "last frame:");
    for (unsigned i = 0; i < last_len && i < 8; i++)
        hb_draw_hex16((int16_t)(6 + 24 * i), 176, last[i], HB_WHITE, HB_BLACK);

    line(HB_SCREEN_H - 22, "tap = cycle mode   vol = exit");
}

/* ---- test cycle --------------------------------------------------------- */
static int g_mode = AAP_MODE_ANC;

static void step(uint32_t now_ms, int *stage_out, int *link_out,
                 aap_battery_t *bat, aap_ears_t *ears,
                 uint32_t *rx_count, uint8_t *last, unsigned *last_len) {
    static uint32_t t_next;
    static uint8_t  tx[AAP_MAX_PKT];
    uint8_t rx[AAP_MAX_PKT];
    unsigned got = 0;

    /* drain whatever arrived */
    while (nano_aap_link_read(rx, sizeof rx, &got) > 0 && got) {
        (*rx_count)++;
        memcpy(last, rx, got < AAP_MAX_PKT ? got : AAP_MAX_PKT);
        *last_len = got < AAP_MAX_PKT ? got : AAP_MAX_PKT;
        aap_mode_t m;
        if (aap_parse_battery(rx, got, bat))      hb_trace_log("AAPBAT", *rx_count, got);
        if (aap_parse_ears(rx, got, ears))        hb_trace_log("AAPEAR", ears->primary, ears->secondary);
        if (aap_parse_mode(rx, got, &m))          hb_trace_log("AAPMOD", (uint32_t)m, *rx_count);
        hb_trace_log("AAPRX", aap_opcode(rx, got), got);
        got = 0;
    }

    if (now_ms < t_next) return;
    t_next = now_ms + 500;

    if (*link_out == 0) {
        int rc = nano_aap_link_open();
        hb_trace_log("AAPOPEN", (uint32_t)rc, 0);
        if (rc == 0) { *link_out = 1; *stage_out = 0; }
        else return;
    }

    unsigned n;
    switch (*stage_out) {
        case 0:
            n = aap_handshake(tx);
            hb_trace_log("AAPHS", nano_aap_link_write(tx, n), n);
            *stage_out = 1;
            break;
        case 1:
            n = aap_unlock_features(tx);
            nano_aap_link_write(tx, n);
            n = aap_subscribe(tx);
            hb_trace_log("AAPSUB", nano_aap_link_write(tx, n), n);
            *stage_out = 2;
            break;
        case 2:
            n = aap_set_mode(tx, (aap_mode_t)g_mode);
            hb_trace_log("AAPSET", (uint32_t)g_mode, nano_aap_link_write(tx, n));
            *stage_out = 3;
            break;
        default:
            break;
    }
}

void aap_probe_main(void) {
    int link = 0, stage = 0;
    uint32_t rx_count = 0, t0 = 0;
    uint8_t last[AAP_MAX_PKT] = {0};
    unsigned last_len = 0;
    aap_battery_t bat = {0};
    aap_ears_t ears = { 0xFF, 0xFF };

    hb_trace_init();
    hb_trace_log("AAPBOOT", 0, 0);

    for (;;) {
        t0 += 16;
        step(t0, &stage, &link, &bat, &ears, &rx_count, last, &last_len);
        render(link, stage, g_mode, &bat, &ears, rx_count, last, last_len);

        if (hb_button_pressed(HB_BTN_VOL_UP) || hb_button_pressed(HB_BTN_VOL_DOWN)) {
            hb_trace_log("AAPEXIT", (uint32_t)stage, rx_count);
            return;  /* harness exits; `start` re-ejects to Home */
        }
        /* tap cycles the mode so a human can watch the AirPods react. Touch API:
         * see sdk/hb_surface_input.c — wire it after G1, when a re-send is meaningful. */
    }
}
