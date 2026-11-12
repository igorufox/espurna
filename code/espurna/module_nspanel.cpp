/*

PROTOTYPE: the NSPanel module of the ESP32 head (docs/modules.md, "ESP32"), env:esp32-modules of
platformio_esp32.ini: nspanel*.cpp as they are (UART protocol, both display drivers, pages, TFT upload).

*/

#include "espurna.h"

#if MODULE_BUILD && defined(ESP32) && NSPANEL_SUPPORT

#include "modules.h"
#include "nspanel.h"

// the monolithic image calls nspanelBootSetup() early, also in the unstable mode; a module starts in stable boots only
ESPURNA_MODULE(nspanel, "nspanel", 60) {
    nspanelBootSetup();
    nspanelSetup();
}

#endif
