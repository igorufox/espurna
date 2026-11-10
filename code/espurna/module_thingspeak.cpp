/*

PROTOTYPE: the thingspeak module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && THINGSPEAK_SUPPORT

#include "modules.h"
#include "thingspeak.h"

ESPURNA_MODULE(thingspeak, "thingspeak", 50) {
#if SENSOR_SUPPORT
    modulesProvide(espurna::modules::Api::TspkEnqueueMagnitude, &tspkEnqueueMagnitude);
#endif
    tspkSetup();
}

#endif
