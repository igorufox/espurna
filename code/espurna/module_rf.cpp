/*

PROTOTYPE: the rf module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && (RFB_SUPPORT)

#include "modules.h"
#include "rfbridge.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_rf.ipp"
#endif

ESPURNA_MODULE(rf, "rf", 50) {
#if WEB_SUPPORT
    modulesUi("rf", ui_rf_data, sizeof(ui_rf_data));
#endif

    // the codes for the RPN rules (they start after this one)
    modulesProvide(espurna::modules::Api::RfbSend, &rfbSend);
    modulesProvide(espurna::modules::Api::RfbOnCode, &rfbOnCode);

    rfbSetup();
}

#endif
