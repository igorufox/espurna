/*

The ntp module (docs/modules.md): the time of the board from NTP servers (ntp.cpp, ntp_driver.cpp: the SNTP of the
SDK, the time zone, the minute / hour ticks). Out of the head (owner 2026-11-12). The head's log and pages, MQTT,
the sensors and the scheduler ask it through the head (ntpSynced, ntpDateTime, ntpOnTick): without it the board has
no time. TLS works without it, except the CA check of the tls module (the validity of a certificate needs the time).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "ntp.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_ntp.ipp"
#endif

ESPURNA_MODULE(ntp, "ntp", 6) {
#if WEB_SUPPORT
    modulesUi("ntp", ui_ntp_data, sizeof(ui_ntp_data));
#endif

    using espurna::modules::Api;

    modulesProvide(Api::NtpOnTick, &ntpOnTick);
    modulesProvide(Api::NtpDateTime, &ntpDateTime);
    modulesProvide(Api::NtpSynced, &ntpSynced);

    ntpSetup();
}

#endif // MODULE_BUILD
