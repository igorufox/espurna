/*

PROTOTYPE: the mdns module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && MDNS_SERVER_SUPPORT

#include "modules.h"
#include "espurna_mdns.h"

// before MQTT (its broker autodiscovery queries mDNS through the head)
ESPURNA_MODULE(mdns, "mdns", 3) {
    using espurna::modules::Api;
    modulesProvide(Api::MdnsRunning, &mdnsRunning);
    modulesProvide(Api::MdnsServiceQuery, &mdnsServiceQuery);
    mdnsServerSetup();
}

#endif
