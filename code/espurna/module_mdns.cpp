/*

PROTOTYPE: the mdns module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && MDNS_SERVER_SUPPORT

#include "modules.h"
#include "espurna_mdns.h"

ESPURNA_MODULE(mdns, "mdns", 20) {
    mdnsServerSetup();
}

#endif
