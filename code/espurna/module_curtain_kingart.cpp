/*

The curtain_kingart module (docs/modules.md): the KingArt curtain motor on a UART port, needs the uart module.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "curtain_kingart.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_curtain_kingart.ipp"
#endif

ESPURNA_MODULE(curtain_kingart, "curtain_kingart", 50) {
#if WEB_SUPPORT
    modulesUi("curtain_kingart", ui_curtain_kingart_data, sizeof(ui_curtain_kingart_data));
#endif

    curtainSetup();
}

#endif // MODULE_BUILD
