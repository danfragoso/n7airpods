/*
 * aap_packets.h — Apple Accessory Protocol framing for AirPods.
 *
 * Facts from LibrePods (kavishdevar) docs: AAP Definitions.md, control_commands.md,
 * captured from AirPods Pro 2 (USB-C) firmware 7A305. Header is always 04 00 04 00,
 * then a little-endian u16 opcode, then payload.
 *
 * Everything here is transport-independent and unit-testable on the host.
 */
#ifndef NANO_AAP_PACKETS_H
#define NANO_AAP_PACKETS_H

#include <stdint.h>
#include <string.h>

#define AAP_MAX_PKT 64

typedef enum {
    AAP_OP_BATTERY      = 0x0004,
    AAP_OP_EAR_DETECT   = 0x0006,
    AAP_OP_CONTROL      = 0x0009,
    AAP_OP_NOTIFY_REQ   = 0x000F,
    AAP_OP_INFO         = 0x001D,
    AAP_OP_SET_FEATURES = 0x004D,
} aap_opcode_t;

/* Control-command identifiers (opcode 0x0009). Source: LibrePods
 * docs/control_commands.md (AirPods Pro 2 USB-C, firmware 7A305). */
#define AAP_CTL_LISTENING_MODE 0x0D
#define AAP_CTL_EAR_DETECT     0x0A
#define AAP_CTL_LISTEN_MODES   0x1A   /* which modes exist in the cycle (bitmask) */
#define AAP_CTL_ONE_BUD_ANC    0x1B
#define AAP_CTL_CHIME_VOLUME   0x1F
#define AAP_CTL_CONV_AWARE     0x28
#define AAP_CTL_AUTO_ANC       0x2E

/* Bitmask values for AAP_CTL_LISTEN_MODES */
#define AAP_MODES_ALLOW_OFF    0x01
#define AAP_MODES_ALLOW_ANC    0x02
#define AAP_MODES_ALLOW_TRANS  0x04
#define AAP_MODES_ALLOW_ADAPT  0x08

/* Listening mode values for AAP_CTL_LISTENING_MODE */
typedef enum {
    AAP_MODE_OFF          = 1,
    AAP_MODE_ANC          = 2,
    AAP_MODE_TRANSPARENCY = 3,
    AAP_MODE_ADAPTIVE     = 4,
} aap_mode_t;

static inline void aap_header(uint8_t *p, uint16_t opcode) {
    p[0] = 0x04; p[1] = 0x00; p[2] = 0x04; p[3] = 0x00;
    p[4] = (uint8_t)(opcode & 0xFF);
    p[5] = (uint8_t)(opcode >> 8);
}

/* Mandatory handshake — without it the AirPods ignore every later packet. 16 bytes. */
static inline unsigned aap_handshake(uint8_t *p) {
    static const uint8_t hs[16] = {
        0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(p, hs, sizeof hs);
    return sizeof hs;
}

/* Subscribe to battery / ear detection / noise-control notifications. 10 bytes. */
static inline unsigned aap_subscribe(uint8_t *p) {
    aap_header(p, AAP_OP_NOTIFY_REQ);
    p[6] = 0xFF; p[7] = 0xFF; p[8] = 0xFF; p[9] = 0xFF;
    return 10;
}

/* Unlock features gated behind Apple OS-version checks (adaptive transparency,
 * conversational awareness during playback). Not needed for ANC/battery. 14 bytes. */
static inline unsigned aap_unlock_features(uint8_t *p) {
    aap_header(p, AAP_OP_SET_FEATURES);
    p[6] = 0xFF; p[7] = 0x00;
    memset(p + 8, 0, 6);
    return 14;
}

/* Set noise control mode. 11 bytes: 04 00 04 00 09 00 0D [mode] 00 00 00
 * Exactly 7 bytes after the header per LibrePods: identifiers with a single
 * value use data1 only, data2..data4 stay zero. Sending more bytes desyncs
 * the accessory's parse of the *next* frame. */
static inline unsigned aap_set_mode(uint8_t *p, aap_mode_t mode) {
    aap_header(p, AAP_OP_CONTROL);
    p[6] = AAP_CTL_LISTENING_MODE;
    p[7] = (uint8_t)mode;
    p[8] = 0x00; p[9] = 0x00; p[10] = 0x00;
    return 11;
}

/* Generic single-value control command, same 11-byte shape as set_mode. */
static inline unsigned aap_control(uint8_t *p, uint8_t id, uint8_t value) {
    aap_header(p, AAP_OP_CONTROL);
    p[6] = id; p[7] = value; p[8] = 0x00; p[9] = 0x00; p[10] = 0x00;
    return 11;
}

/* Which listening modes the stem/cycle offers. Bitmask of AAP_MODES_ALLOW_*.
 * Adaptive must be included here or the AirPods will refuse mode 4. */
static inline unsigned aap_set_allowed_modes(uint8_t *p, uint8_t mask) {
    return aap_control(p, AAP_CTL_LISTEN_MODES, mask);
}

/* Single-bud ANC. One AirPod cannot do ANC unless this is enabled (0x01). */
static inline unsigned aap_set_one_bud_anc(uint8_t *p, int enabled) {
    return aap_control(p, AAP_CTL_ONE_BUD_ANC, enabled ? 0x01 : 0x02);
}

/* Mode-switch chime volume, 0..100. */
static inline unsigned aap_set_chime_volume(uint8_t *p, uint8_t volume) {
    return aap_control(p, AAP_CTL_CHIME_VOLUME, volume > 100 ? 100 : volume);
}

/* ---- parsing ------------------------------------------------------------ */

typedef struct { uint8_t left, right, case_, have_left, have_right, have_case; } aap_battery_t;
typedef struct { uint8_t primary, secondary; } aap_ears_t;

static inline uint16_t aap_opcode(const uint8_t *p, unsigned len) {
    if (len < 6) return 0xFFFF;
    return (uint16_t)(p[4] | (p[5] << 8));
}

/* Battery report: 04 00 04 00 04 00 [count] ([component] 01 [level] [status] 01)* */
static inline int aap_parse_battery(const uint8_t *p, unsigned len, aap_battery_t *out) {
    if (aap_opcode(p, len) != AAP_OP_BATTERY || len < 7) return 0;
    unsigned n = p[6], i = 7;
    memset(out, 0, sizeof *out);
    for (unsigned k = 0; k < n && i + 4 < len; k++, i += 5) {   /* 5-byte blocks */
        uint8_t comp = p[i], level = p[i + 2];
        if      (comp == 0x04) { out->left  = level; out->have_left  = 1; }
        else if (comp == 0x02) { out->right = level; out->have_right = 1; }
        else if (comp == 0x08) { out->case_ = level; out->have_case  = 1; }
    }
    return 1;
}

/* Ear detection: 04 00 04 00 06 00 [primary] [secondary]  (00 in, 01 out, 02 case) */
static inline int aap_parse_ears(const uint8_t *p, unsigned len, aap_ears_t *out) {
    if (aap_opcode(p, len) != AAP_OP_EAR_DETECT || len < 8) return 0;
    out->primary = p[6];
    out->secondary = p[7];
    return 1;
}

/* Noise-control state notification: identical frame to the set command. */
static inline int aap_parse_mode(const uint8_t *p, unsigned len, aap_mode_t *out) {
    if (aap_opcode(p, len) != AAP_OP_CONTROL || len < 8) return 0;
    if (p[6] != AAP_CTL_LISTENING_MODE) return 0;
    if (p[7] < AAP_MODE_OFF || p[7] > AAP_MODE_ADAPTIVE) return 0;
    *out = (aap_mode_t)p[7];
    return 1;
}

/* Conversational-awareness *state*, sent once after the notification request:
 * 04 00 04 00 09 00 28 [status] 00 00 00 ; 01 enabled, 02 disabled. */
static inline int aap_parse_conv_aware(const uint8_t *p, unsigned len, int *enabled) {
    if (aap_opcode(p, len) != AAP_OP_CONTROL || len < 8) return 0;
    if (p[6] != AAP_CTL_CONV_AWARE) return 0;
    if (p[7] != 0x01 && p[7] != 0x02) return 0;
    *enabled = (p[7] == 0x01);
    return 1;
}

#endif /* NANO_AAP_PACKETS_H */
