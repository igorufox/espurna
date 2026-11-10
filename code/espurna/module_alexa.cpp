/*

PROTOTYPE: the alexa module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && ALEXA_SUPPORT

#include "modules.h"
#include "alexa.h"

ESPURNA_MODULE(alexa, "alexa", 50) {
    // the Matter module pauses Alexa while a controller commissions (through the head, modules_api_esp8266.cpp)
    modulesProvide(espurna::modules::Api::AlexaPause, &alexaPause);
    alexaSetup();
}

#endif
