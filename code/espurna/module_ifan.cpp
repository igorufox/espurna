/*

The ifan module (docs/modules.md): the Sonoff iFan02 / iFan03 fan speeds on its relays, needs the relay module.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "relay.h"
#include "button.h"
#include "fan.h"

ESPURNA_MODULE(ifan, "ifan", 50) {
    using espurna::modules::Api;

    // the relays of relayProvider "fan" (the relay module started first: it needs it) and the fan buttons
    modulesProvide(Api::FanMakeRelayProvider, &fanMakeRelayProvider);
    modulesProvide(Api::FanSpeed, static_cast<void(*)(FanSpeed)>(&fanSpeed));
    relayProvidersReady();

    fanSetup();
}

#endif // MODULE_BUILD
