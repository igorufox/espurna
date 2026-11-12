/*

The led module (docs/modules.md): the LEDs of the board (led.cpp: modes, patterns, the relay and Wi-Fi states).
Out of the head (owner 2026-11-12), on by default in the initial images.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "led.h"

ESPURNA_MODULE(led, "led", 13) {
    ledSetup();
}

#endif // MODULE_BUILD
