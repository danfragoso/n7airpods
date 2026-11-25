/* aap_models.h — which AirPods exist and what each one can do (the capability table).
 *
 * The page shows only the rows a model supports. Models are recognised, in order of trust, by:
 *   1. the BLE proximity model id (from the advertisements; ids as in LibrePods),
 *   2. the model number the AirPods report over AAP (A2084, A3048, ...),
 *   3. words in their name ("Pro", "Max"),
 * and anything unknown falls back to the basics (battery + ear detection) rather than offering
 * controls that may not exist.
 *
 * Feature | 1 | 2 | 3 | 4 | 4 ANC | Pro | Pro 2/3 | Max
 * --------+---+---+---+---+-------+-----+-------+-----
 * case    | x | x | x | x |   x   |  x  |   x   |  -
 * ear det | x | x | x | x |   x   |  x  |   x   |  x
 * press&hold|-| - | x | x |   x   |  x  |   x   |  -
 * NC modes| - | - | - | - |   x   |  x  |   x   |  x      (Off / Transparency / Noise Cancellation)
 * adaptive| - | - | - | - |   x   |  -  |   x   |  -
 * convers.| - | - | - | - |   x   |  -  |   x   |  -
 * 1-bud NC| - | - | - | - |   x   |  x  |   x   |  -
 * case tone| -| - | - | - |   x   |  -  |   x   |  -
 */
#ifndef AAP_MODELS_H
#define AAP_MODELS_H

#include <stdint.h>

#define AF_CASE       (1u << 0)   /* has a charging case with its own battery */
#define AF_EAR_DETECT (1u << 1)
#define AF_PRESS_HOLD (1u << 2)   /* press-and-hold actions per bud           */
#define AF_NC_MODES   (1u << 3)   /* Off / Transparency / Noise Cancellation  */
#define AF_ADAPTIVE   (1u << 4)   /* adds Adaptive to the modes               */
#define AF_CONV_AWARE (1u << 5)   /* Conversation Awareness                   */
#define AF_ONE_BUD_NC (1u << 6)   /* Noise Cancellation with One AirPod       */
#define AF_CASE_TONE  (1u << 7)   /* in-case charging sounds                  */
#define AF_SINGLE     (1u << 8)   /* one battery (AirPods Max), no L/R/case   */

typedef struct {
    uint16_t ble_id;              /* BLE proximity model id, 0 = none          */
    const char *name;             /* marketing name                            */
    const char *anums;            /* ",A2084,A2083," model numbers (comma-delimited, both ends) */
    uint16_t feat;                /* AF_* bits                                 */
    uint8_t art;                  /* picture set in aap_art.c (1..9), 0 = draw a generic pair */
} aap_model_t;

/* Look a model up. Any argument may be 0/NULL/"". Never returns NULL (falls back to "AirPods"). */
const aap_model_t *aap_model_find(unsigned ble_id, const char *model_number, const char *name);

#endif
