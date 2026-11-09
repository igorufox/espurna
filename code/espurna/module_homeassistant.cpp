/*

PROTOTYPE: the homeassistant module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && HOMEASSISTANT_SUPPORT

#include "modules.h"
#include "homeassistant.h"

ESPURNA_MODULE(homeassistant, "homeassistant", 50) {
    haSetup();
}

#endif
