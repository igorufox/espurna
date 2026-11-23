/*

The nextion module (docs/modules.md): a Nextion display on a UART port, the NSPanel's (nextion_*.cpp: the UART, the
detection of its TFT, the OHpanel and Lovelace drivers, the TFT upload). It shows the tile pages of the screen module
(screen_pages.h) and is a panel of the screen with pages of its own (the QR page of the screen on it).

*/

#include "espurna.h"

#if MODULE_BUILD && NEXTION_SUPPORT

#include "modules.h"
#include "nextion.h"

// the monolithic image calls nextionBootSetup() early, also in the unstable mode; a module starts in stable boots only
#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_nextion.ipp"
#endif

ESPURNA_MODULE(nextion, "nextion", 60) {
#if WEB_SUPPORT
    modulesUi("nextion", ui_nextion_data, sizeof(ui_nextion_data));
#endif

    nextionBootSetup();
    nextionSetup();
}

#endif
