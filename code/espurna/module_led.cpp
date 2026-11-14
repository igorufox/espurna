/*

The led module (docs/modules.md): the LEDs of the board (led.cpp: modes, patterns, the relay and Wi-Fi states).
Out of the head (owner 2026-11-12), on by default in the initial images.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "led.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_led.ipp"
#endif

ESPURNA_MODULE(led, "led", 13) {
#if WEB_SUPPORT
    modulesUi("led", ui_led_data, sizeof(ui_led_data));
#endif

    ledSetup();
}

#endif // MODULE_BUILD
