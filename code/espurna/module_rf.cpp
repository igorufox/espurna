/*

PROTOTYPE: the rf module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && (RFB_SUPPORT)

#include "modules.h"
#include "rfbridge.h"

ESPURNA_MODULE(rf, "rf", 50) {
    rfbSetup();
}

#endif
