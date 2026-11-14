/*

The rpnrules module (docs/modules.md): RPN rules (RPNlib): small programs on the relays, the sensors, MQTT and the time; before the relays (it follows their changes from the start).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "rpnrules.h"

// after the rf module (its codes: rfbOnCode() reaches it only once it has started)
#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_rpnrules.ipp"
#endif

ESPURNA_MODULE(rpnrules, "rpnrules", 60) {
#if WEB_SUPPORT
    modulesUi("rpnrules", ui_rpnrules_data, sizeof(ui_rpnrules_data));
#endif

    rpnSetup();
}

#endif // MODULE_BUILD
