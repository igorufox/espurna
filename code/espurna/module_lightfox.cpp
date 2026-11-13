/*

The lightfox module (docs/modules.md): the LightFox dual relay board (its MCU on a UART port), needs the relay, button and uart modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "relay.h"
#include "button.h"
#include "lightfox.h"

ESPURNA_MODULE(lightfox, "lightfox", 50) {
    using espurna::modules::Api;

    // its relays and buttons (relayProvider / btnProvider "lightfox"; the relay and button modules started first)
    modulesProvide(Api::LightfoxMakeRelayProvider, &lightfoxMakeRelayProvider);
    modulesProvide(Api::LightfoxMakeButtonPin, &lightfoxMakeButtonPin);
    relayProvidersReady();
    buttonProvidersReady();

    lightfoxSetup();
}

#endif // MODULE_BUILD
