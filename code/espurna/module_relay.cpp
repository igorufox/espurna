/*

The relay module (docs/modules.md): the relays of the board (relay.cpp: GPIO, the providers of the other modules,
the boot states, pulse / timers, MQTT / WebSocket / API of the relays). Out of the head (owner 2026-11-12): a board
gets it with the modules on by default of its initial image. Starts after light (the light provider of a relay is the
light module's state) and before the modules that follow the relays (button, led, alexa, ...).

Provides through the head (modules_api.cpp) what the modules that work without relays too call: the count, the status,
the toggle, the payloads, the status callbacks.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "relay.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_relay.ipp"
#endif

ESPURNA_MODULE(relay, "relay", 12) {
#if WEB_SUPPORT
    modulesUi("relay", ui_relay_data, sizeof(ui_relay_data));
#endif

    using espurna::modules::Api;

    modulesProvide(Api::RelayCount, &relayCount);
    modulesProvide(Api::RelayStatusGet, static_cast<RelayStatus(*)(size_t)>(&relayStatus));
    modulesProvide(Api::RelayStatusSet, static_cast<bool(*)(size_t, bool)>(&relayStatus));
    modulesProvide(Api::RelayStatusAll, static_cast<bool(*)()>(&relayStatus));
    modulesProvide(Api::RelayToggle, &relayToggle);
    modulesProvide(Api::RelayParsePayload, &relayParsePayload);
    modulesProvide(Api::RelayPayload, &relayPayload);
    modulesProvide(Api::RelayOnStatusChange, &relayOnStatusChange);
    modulesProvide(Api::RelayOnStatusNotify, &relayOnStatusNotify);
    modulesProvide(Api::RelayOnReady, &relayOnReady);
    modulesProvide(Api::RelayBtnGpio, &relayBtnGpio);

    relaySetup();
}

#endif // MODULE_BUILD
