/*

PROTOTYPE: the thingspeak module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && THINGSPEAK_SUPPORT

#include "modules.h"
#include "thingspeak.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_thingspeak.ipp"
#endif

ESPURNA_MODULE(thingspeak, "thingspeak", 50) {
#if WEB_SUPPORT
    modulesUi("thingspeak", ui_thingspeak_data, sizeof(ui_thingspeak_data));
#endif

#if SENSOR_SUPPORT
    modulesProvide(espurna::modules::Api::TspkEnqueueMagnitude, &tspkEnqueueMagnitude);
#endif
    tspkSetup();
}

#endif
