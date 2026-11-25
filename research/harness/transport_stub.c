/*
 * transport_stub.c — placeholder BLUEmagic transport.
 *
 * Phase 1 replaces these three bodies with real calls once tools/survey.sh has
 * named the entry point. Keeping them behind this seam means aap_probe.c and
 * aap_packets.h are finished, host-testable code today.
 *
 * Candidate shapes, in the order FEASIBILITY.md ranks them:
 *
 *  A) clone the AVDTP channel open (AVDTP is L2CAP PSM 0x0019) and pass 0x1001
 *     instead of the immediate 0x19, reusing the same completion callback type;
 *  B) call an exported stack entry point if one exists (name TBD by G0), e.g.
 *        int OI_L2CAP_Connect(uint16_t psm, void (*cb)(...), void *ctx);
 *  C) patch a dispatch table so a new PSM lands in our handler.
 *
 * Until then: -ENOSYS, so the probe renders "not open" and the trace records
 * AAPOPEN with a failure code. That is the expected pre-G1 result and proves
 * the plumbing on-device.
 */
#include <stdint.h>

#define ENOSYS 38

int nano_aap_link_open(void) {
    return -ENOSYS;   /* no transport yet — see banner comment */
}

int nano_aap_link_write(const uint8_t *buf, unsigned len) {
    (void)buf; (void)len;
    return -ENOSYS;
}

int nano_aap_link_read(uint8_t *buf, unsigned max, unsigned *got) {
    (void)buf; (void)max;
    if (got) *got = 0;
    return 0;
}
