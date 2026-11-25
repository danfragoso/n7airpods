/* Host-test shim for integration/aap_le.c: just the bits it needs from the device SDK. */
#ifndef HB_SDK_SHIM_H
#define HB_SDK_SHIM_H
#include <stdint.h>
#define AAP_LE_HOST_TEST 1
uint32_t hb_time_uptime_ms(void);
#endif
