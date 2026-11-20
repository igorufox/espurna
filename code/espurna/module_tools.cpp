/*

The tools module (docs/modules.md): what the modules use and the head does not need (owner 2026-11-18). Today the JSON
of ESPurna (json.h, json.cpp) and its helpers of the WebUI (ws_utils.cpp), the date and time calls (datetime.cpp: NTP,
the scheduler, the WebUI). The modules that use it
have it in their needs column (config/head.h) and are linked against it directly.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"

ESPURNA_MODULE(tools, "tools", 1) {
}

#endif // MODULE_BUILD
