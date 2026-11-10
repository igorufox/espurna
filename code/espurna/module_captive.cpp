/*

The captive portal module (docs/modules.md), every chip: the DNS server and the connectivity check redirects of the AP

*/

#include "espurna.h"

#if MODULE_BUILD && WIFI_AP_CAPTIVE_SUPPORT

#include "modules.h"
#include "captive.h"

ESPURNA_MODULE(captive, "captive", 50) {
    captiveSetup();
}

#endif
