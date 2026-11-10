/*

PROTOTYPE: the domoticz module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && DOMOTICZ_SUPPORT

#include "modules.h"
#include "domoticz.h"

ESPURNA_MODULE(domoticz, "domoticz", 50) {
#if SENSOR_SUPPORT
    modulesProvide(espurna::modules::Api::DomoticzSendMagnitude, &domoticzSendMagnitude);
#endif
    domoticzSetup();
}

#endif
