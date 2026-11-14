/*

PROTOTYPE: the Matter module (docs/modules.md): matter.cpp + the backend of the chip (arch/beken/matter_beken.cpp and
libMatterApp.a on the Beken, env:beken-modules of platformio_beken.ini), linked against a head that has what
the Matter stack needs (the Beken: lwIP with IPv6 and the library's lwipopts.h)

*/

#include "espurna.h"

#if MODULE_BUILD && MATTER_SUPPORT

#include "modules.h"
#include "espurna_matter.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_matter.ipp"
#endif

ESPURNA_MODULE(matter, "matter", 60) {
#if WEB_SUPPORT
    modulesUi("matter", ui_matter_data, sizeof(ui_matter_data));
#endif

    matterSetup();
}

#endif
