/*

PROTOTYPE: the thingspeak module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && THINGSPEAK_SUPPORT

#include "modules.h"
#include "thingspeak.h"

ESPURNA_MODULE(thingspeak, "thingspeak", 50) {
    modulesProvide(espurna::modules::Api::TspkEnqueueMagnitude, &tspkEnqueueMagnitude);
    tspkSetup();
}

#endif
