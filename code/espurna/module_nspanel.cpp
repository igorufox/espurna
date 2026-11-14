/*

PROTOTYPE: the NSPanel module of the ESP32 head (docs/modules.md, "ESP32"), env:esp32-modules of
platformio_esp32.ini: nspanel*.cpp as they are (UART protocol, both display drivers, pages, TFT upload).

*/

#include "espurna.h"

#if MODULE_BUILD && NSPANEL_SUPPORT

#include "modules.h"
#include "nspanel.h"

// the monolithic image calls nspanelBootSetup() early, also in the unstable mode; a module starts in stable boots only
#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_nspanel.ipp"
#endif

ESPURNA_MODULE(nspanel, "nspanel", 60) {
#if WEB_SUPPORT
    modulesUi("nspanel", ui_nspanel_data, sizeof(ui_nspanel_data));
#endif

    nspanelBootSetup();
    nspanelSetup();
}

#endif
