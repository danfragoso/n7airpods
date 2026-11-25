#include "aap_models.h"

#define BASIC   (AF_CASE | AF_EAR_DETECT)
#define PRESS   (BASIC | AF_PRESS_HOLD)
#define NC1     (PRESS | AF_NC_MODES | AF_ONE_BUD_NC)                   /* AirPods Pro                */
#define NC2     (NC1 | AF_ADAPTIVE | AF_CONV_AWARE | AF_CASE_TONE)      /* Pro 2, AirPods 4 (ANC)     */

static const aap_model_t k_models[] = {
    { 0x0220, "AirPods (1st generation)",    ",A1523,A1722,",          BASIC, 1 },
    { 0x0f20, "AirPods (2nd generation)",    ",A2032,A2031,",          BASIC, 2 },
    { 0x1320, "AirPods (3rd generation)",    ",A2565,A2564,",          PRESS, 3 },
    { 0x1920, "AirPods 4",                   ",A3050,A3053,",          PRESS, 4 },
    { 0x1b20, "AirPods 4 (ANC)",             ",A3054,A3055,",          NC2,   4 },
    { 0x0e20, "AirPods Pro",                 ",A2084,A2083,",          NC1,   5 },
    { 0x1420, "AirPods Pro 2",               ",A2931,A2699,A2698,",    NC2,   6 },
    { 0x2420, "AirPods Pro 2 (USB-C)",       ",A3047,A3048,A3049,",    NC2,   6 },
    { 0x0a20, "AirPods Max",                 ",A2096,",                AF_EAR_DETECT | AF_NC_MODES | AF_SINGLE, 8 },
    { 0x1f20, "AirPods Max (USB-C)",         ",A3184,",                AF_EAR_DETECT | AF_NC_MODES | AF_SINGLE, 9 },
    { 0x2720, "AirPods Pro 3",               "",                       NC2,   7 },   /* model numbers not confirmed: BLE id only */
};
static const aap_model_t k_unknown = { 0, "AirPods", "", BASIC, 0 };

/* Case-insensitive substring test (the device's freestanding string.h has no strstr). */
static int ci_contains(const char *h, const char *n)
{
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Is `num` one of the comma-delimited entries of `list` (",A2084,A2083,")? */
static int anum_in(const char *list, const char *num)
{
    for (const char *p = list; *p; ) {
        if (*p == ',') { p++; continue; }
        const char *q = num;
        const char *s = p;
        while (*s && *s != ',' && *q && *s == *q) { s++; q++; }
        if ((!*s || *s == ',') && !*q) return 1;
        while (*p && *p != ',') p++;
    }
    return 0;
}

const aap_model_t *aap_model_find(unsigned ble_id, const char *model_number, const char *name)
{
    unsigned n = sizeof k_models / sizeof k_models[0];
    if (ble_id)
        for (unsigned i = 0; i < n; i++) if (k_models[i].ble_id == ble_id) return &k_models[i];
    if (model_number && model_number[0])
        for (unsigned i = 0; i < n; i++) if (anum_in(k_models[i].anums, model_number)) return &k_models[i];
    if (name && name[0]) {                 /* last resort: words in the name */
        if (ci_contains(name, "max"))   return &k_models[8];
        if (ci_contains(name, "pro 2")) return &k_models[7];
        if (ci_contains(name, "pro"))   return &k_models[5];
    }
    return &k_unknown;
}
