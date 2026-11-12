/*

The garland module (docs/modules.md): LED strip effects on WS2812 (Adafruit NeoPixel).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "garland.h"

ESPURNA_MODULE(garland, "garland", 50) {
    garlandSetup();
}

#endif // MODULE_BUILD
