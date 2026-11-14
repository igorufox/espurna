/*

PROTOTYPE: the scheduler module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && SCHEDULER_SUPPORT

#include "modules.h"
#include "scheduler.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_scheduler.ipp"
#endif

ESPURNA_MODULE(scheduler, "scheduler", 50) {
#if WEB_SUPPORT
    modulesUi("scheduler", ui_scheduler_data, sizeof(ui_scheduler_data));
#endif

    schSetup();
}

#endif
