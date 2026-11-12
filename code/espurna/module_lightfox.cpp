/*

The lightfox module (docs/modules.md): the LightFox dual relay board (its MCU on a UART port), needs the relay, button and uart modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "lightfox.h"

ESPURNA_MODULE(lightfox, "lightfox", 50) {
    lightfoxSetup();
}

#endif // MODULE_BUILD
