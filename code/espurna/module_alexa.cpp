/*

PROTOTYPE: the alexa module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && ALEXA_SUPPORT

#include "modules.h"
#include "alexa.h"

ESPURNA_MODULE(alexa, "alexa", 50) {
    alexaSetup();
}

#endif
