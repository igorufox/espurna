/*

The lightfox module (docs/modules.md): the LightFox dual relay board (its MCU on a UART port), needs the relay, button and uart modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "relay.h"
#include "button.h"
#include "lightfox.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_lightfox.ipp"
#endif

ESPURNA_MODULE(lightfox, "lightfox", 50) {
#if WEB_SUPPORT
    modulesUi("lightfox", ui_lightfox_data, sizeof(ui_lightfox_data));
#endif

    using espurna::modules::Api;

    // its relays and buttons (relayProvider / btnProvider "lightfox"; the relay and button modules started first)
    modulesProvide(Api::LightfoxMakeRelayProvider, &lightfoxMakeRelayProvider);
    modulesProvide(Api::LightfoxMakeButtonPin, &lightfoxMakeButtonPin);
    relayProvidersReady();
    buttonProvidersReady();

    lightfoxSetup();
}

#endif // MODULE_BUILD
