/*

PROTOTYPE: the homeassistant module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && HOMEASSISTANT_SUPPORT

#include "modules.h"
#include "homeassistant.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_homeassistant.ipp"
#endif

ESPURNA_MODULE(homeassistant, "homeassistant", 50) {
#if WEB_SUPPORT
    modulesUi("homeassistant", ui_homeassistant_data, sizeof(ui_homeassistant_data));
#endif

    haSetup();
}

#endif
