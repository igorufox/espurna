/*

The button module (docs/modules.md): the buttons and switches of the board (button.cpp, the AC switch detection,
the debounce), their actions on the relays and lights, MQTT / WebSocket. Out of the head (owner 2026-11-12), on by
default in the initial images. Calls the relays through the head: a board with buttons only works too.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "button.h"

ESPURNA_MODULE(button, "button", 13) {
    using espurna::modules::Api;

    modulesProvide(Api::ButtonSetCustomAction, &buttonSetCustomAction);

    buttonSetup();
}

#endif // MODULE_BUILD
