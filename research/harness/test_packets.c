/*
 * test_packets.c — host-side proof that the AAP frames are byte-correct.
 * No device, no toolchain:  cc -o /tmp/t harness/test_packets.c && /tmp/t
 */
#include <stdio.h>
#include <string.h>
#include "aap_packets.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); fails++; } } while (0)

static void expect(const uint8_t *got, unsigned n, const uint8_t *want, unsigned wn, const char *name) {
    if (n != wn || memcmp(got, want, wn)) {
        printf("FAIL %s\n  got  ", name);
        for (unsigned i = 0; i < n; i++) printf("%02x ", got[i]);
        printf("\n  want ");
        for (unsigned i = 0; i < wn; i++) printf("%02x ", want[i]);
        printf("\n");
        fails++;
    } else {
        printf("ok   %s (%u bytes)\n", name, n);
    }
}

int main(void) {
    uint8_t p[AAP_MAX_PKT];

    { unsigned n = aap_handshake(p);
      const uint8_t w[16] = {0,0,4,0,1,0,2,0,0,0,0,0,0,0,0,0};
      expect(p, n, w, sizeof w, "handshake"); }

    { unsigned n = aap_set_mode(p, AAP_MODE_ANC);
      const uint8_t w[11] = {0x04,0,0x04,0,0x09,0,0x0D,0x02,0,0,0};
      expect(p, n, w, sizeof w, "set ANC"); }

    { unsigned n = aap_set_mode(p, AAP_MODE_TRANSPARENCY);
      CHECK(p[7] == 0x03, "transparency byte"); (void)n; }

    { unsigned n = aap_set_mode(p, AAP_MODE_OFF);
      CHECK(p[7] == 0x01, "off byte"); (void)n; }

    { unsigned n = aap_subscribe(p);
      const uint8_t w[10] = {0x04,0,0x04,0,0x0F,0,0xFF,0xFF,0xFF,0xFF};
      expect(p, n, w, sizeof w, "subscribe"); }

    { unsigned n = aap_unlock_features(p);
      const uint8_t w[14] = {0x04,0,0x04,0,0x4D,0,0xFF,0x00,0,0,0,0,0,0};
      expect(p, n, w, sizeof w, "unlock features"); }

    /* Every control command must be exactly 11 bytes: 7 bytes after the header.
     * A longer frame was one of the defects in the abandoned build. */
    { unsigned n = aap_set_allowed_modes(p, AAP_MODES_ALLOW_OFF | AAP_MODES_ALLOW_ANC
                                              | AAP_MODES_ALLOW_TRANS | AAP_MODES_ALLOW_ADAPT);
      const uint8_t w[11] = {0x04,0,0x04,0,0x09,0,0x1A,0x0F,0,0,0};
      expect(p, n, w, sizeof w, "allowed modes = all"); }

    { unsigned n = aap_set_one_bud_anc(p, 1);
      const uint8_t w[11] = {0x04,0,0x04,0,0x09,0,0x1B,0x01,0,0,0};
      expect(p, n, w, sizeof w, "one-bud ANC on"); }

    { unsigned n = aap_set_one_bud_anc(p, 0);
      CHECK(p[7] == 0x02, "one-bud ANC off = 0x02"); (void)n; }

    { unsigned n = aap_set_chime_volume(p, 60);
      const uint8_t w[11] = {0x04,0,0x04,0,0x09,0,0x1F,0x3C,0,0,0};
      expect(p, n, w, sizeof w, "chime volume 60"); }

    { unsigned n = aap_set_chime_volume(p, 200);
      CHECK(p[7] == 100, "chime volume clamps to 100"); (void)n; }

    { const uint8_t rx[11] = {0x04,0x00,0x04,0x00,0x09,0x00,0x28,0x01,0,0,0};
      int en = -1;
      CHECK(aap_parse_conv_aware(rx, sizeof rx, &en) && en == 1, "conv aware enabled"); }

    /* parse a real battery report captured from AirPods Pro 2 (LibrePods doc example) */
    { const uint8_t rx[23] = {0x04,0x00,0x04,0x00,0x04,0x00,0x03,
                              0x02,0x01,0x64,0x02,0x01,
                              0x04,0x01,0x63,0x01,0x01,
                              0x08,0x01,0x11,0x02,0x01};
      aap_battery_t b;
      CHECK(aap_parse_battery(rx, sizeof rx, &b), "battery parses");
      CHECK(b.have_right && b.right == 100, "right 100%");
      CHECK(b.have_left  && b.left  == 99,  "left 99%");
      CHECK(b.have_case  && b.case_ == 17,  "case 17%");
      CHECK(aap_opcode(rx, sizeof rx) == AAP_OP_BATTERY, "opcode 0x04"); }

    /* ear detection: primary out, secondary in */
    { const uint8_t rx[8] = {0x04,0x00,0x04,0x00,0x06,0x00,0x01,0x00};
      aap_ears_t e;
      CHECK(aap_parse_ears(rx, sizeof rx, &e), "ears parse");
      CHECK(e.primary == 1 && e.secondary == 0, "primary out, secondary in"); }

    /* mode notification is the same frame shape as the command */
    { const uint8_t rx[11] = {0x04,0x00,0x04,0x00,0x09,0x00,0x0D,0x03,0,0,0};
      aap_mode_t m = 0;
      CHECK(aap_parse_mode(rx, sizeof rx, &m) && m == AAP_MODE_TRANSPARENCY, "mode notify"); }

    /* a control command for a different identifier must not be read as a mode */
    { const uint8_t rx[11] = {0x04,0x00,0x04,0x00,0x09,0x00,0x1B,0x02,0,0,0};
      aap_mode_t m = 0;
      CHECK(!aap_parse_mode(rx, sizeof rx, &m), "one-bud-ANC ignored by mode parser"); }

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails != 0;
}
