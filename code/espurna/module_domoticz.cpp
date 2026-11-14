/*

PROTOTYPE: the domoticz module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && DOMOTICZ_SUPPORT

#include "modules.h"
#include "domoticz.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_domoticz.ipp"
#endif

ESPURNA_MODULE(domoticz, "domoticz", 50) {
#if WEB_SUPPORT
    modulesUi("domoticz", ui_domoticz_data, sizeof(ui_domoticz_data));
#endif

#if SENSOR_SUPPORT
    modulesProvide(espurna::modules::Api::DomoticzSendMagnitude, &domoticzSendMagnitude);
#endif
    domoticzSetup();
}

#endif
