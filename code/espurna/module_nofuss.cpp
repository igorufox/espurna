/*

The nofuss module (docs/modules.md): NoFUSS: asks its server for a newer firmware, the update through the head's OTA by URL.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "nofuss.h"

ESPURNA_MODULE(nofuss, "nofuss", 50) {
    nofussSetup();
}

#endif // MODULE_BUILD
