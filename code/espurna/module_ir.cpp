/*

The ir module (docs/modules.md): infrared receive and send (IRremoteESP8266): codes on MQTT, relay buttons of a remote.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "ir.h"

ESPURNA_MODULE(ir, "ir", 50) {
    irSetup();
}

#endif // MODULE_BUILD
