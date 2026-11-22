/*

The thermostat module (docs/modules.md): a thermostat on a relay and a temperature of the sensors (the screen is the oled module); needs the relay, sensors and ntp modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "thermostat.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_thermostat.ipp"
#endif

ESPURNA_MODULE(thermostat, "thermostat", 50) {
#if WEB_SUPPORT
    modulesUi("thermostat", ui_thermostat_data, sizeof(ui_thermostat_data));
#endif

    thermostatSetup();
}

#endif // MODULE_BUILD
