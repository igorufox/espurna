/*

PROTOTYPE: the domoticz module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && DOMOTICZ_SUPPORT

#include "modules.h"
#include "domoticz.h"

ESPURNA_MODULE(domoticz, "domoticz", 50) {
    modulesProvide(espurna::modules::Api::DomoticzSendMagnitude, &domoticzSendMagnitude);
    domoticzSetup();
}

#endif
