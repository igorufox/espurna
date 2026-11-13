/*

The tuya module (docs/modules.md): the Tuya MCU protocol on a UART port: the switches and dimmers of the MCU as relays and light channels, needs the relay, light and uart modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "relay.h"
#include "button.h"
#include "tuya.h"

ESPURNA_MODULE(tuya, "tuya", 50) {
    using espurna::modules::Api;

    // the relays of relayProvider "tuya" (the relay module started first: it needs it)
    modulesProvide(Api::TuyaMakeRelayProvider, &tuya::makeRelayProvider);
    relayProvidersReady();

    tuya::setup();
}

#endif // MODULE_BUILD
