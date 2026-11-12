/*

The ifan module (docs/modules.md): the Sonoff iFan02 / iFan03 fan speeds on its relays, needs the relay module.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "fan.h"

ESPURNA_MODULE(ifan, "ifan", 50) {
    fanSetup();
}

#endif // MODULE_BUILD
