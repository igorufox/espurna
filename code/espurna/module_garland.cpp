/*

The garland module (docs/modules.md): LED strip effects on WS2812 (Adafruit NeoPixel).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "garland.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_garland.ipp"
#endif

ESPURNA_MODULE(garland, "garland", 50) {
#if WEB_SUPPORT
    modulesUi("garland", ui_garland_data, sizeof(ui_garland_data));
#endif

    garlandSetup();
}

#endif // MODULE_BUILD
