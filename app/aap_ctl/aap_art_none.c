/* aap_art_none.c - used instead of aap_art.c when building with APPLE_ART=0 (./install.sh --no-apple-art).
 * No pictures: the page draws plain, generic stand-ins for the buds and the case. See docs/ASSETS.md. */
#include "aap_art.h"

const aap_art_t *aap_art_get(unsigned id)
{
    (void)id;
    return 0;
}
