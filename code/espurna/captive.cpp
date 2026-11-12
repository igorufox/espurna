/*

CAPTIVE PORTAL MODULE (docs/modules.md), every chip

While the access point is on (and wifiApCaptive is set) the DNS server answers every name with the AP address, and the
connectivity checks of phones and PCs (/generate_204, /connecttest.txt, /hotspot-detect.html ...) get a redirect to
the board, so a client of the AP shows the board's page by itself. Without the module the AP works the same, the page
is at the AP address (192.168.4.1).

The DNS server is lib/DNSServer (on its EspurnaUdp, asynchronous: no loop polling), linked into the module only.

*/

#include "espurna.h"

#if MODULE_BUILD && WIFI_AP_CAPTIVE_SUPPORT

#include "captive.h"
#include "espurna_wifi.h"
#include "system.h"
#include "utils.h"
#include "web.h"

#include <DNSServer.h>

namespace espurna {
namespace captive {
namespace {

namespace internal {

DNSServer dns;
IPAddress running;

} // namespace internal

// on every AP start / stop (wifi Event::Mode) and settings reload: DNS on the current AP address, or off
void update() {
    const auto ip = (wifiConnectable() && wifiApCaptive())
        ? wifiApIp()
        : IPAddress();

    if (ip == internal::running) {
        return;
    }

    internal::dns.stop();
    internal::running = IPAddress();

    if (static_cast<uint32_t>(ip) == 0) {
        return;
    }

    internal::dns.setErrorReplyCode(DNSReplyCode::NoError);
    if (internal::dns.start(53, "*", ip)) {
        internal::running = ip;
    }
}

void onWifiEvent(wifi::Event event) {
    if (wifi::Event::Mode == event) {
        update();
    }
}

// to the page of the access point while it is on, else 404
void onRequest(WebRequest& request) {
    if (wifiConnectable()) {
        const String location = String(F("http://")) + ipToString(wifiApIp());
        request.addHeader("Location", location.c_str());
        request.send(302, "", "");
        return;
    }

    request.send(404, "text/plain", "Not Found");
}

} // namespace

void setup() {
    auto& server = webServer();
    server.on("/generate_204", WebMethod::Any, onRequest);
    server.on("/fwlink", WebMethod::Any, onRequest);
    // Windows 10+ and Apple connectivity checks, any other host name gets 404 while the AP is on
    server.on("/connecttest.txt", WebMethod::Any, onRequest);
    server.on("/hotspot-detect.html", WebMethod::Any, onRequest);

    wifiRegister(onWifiEvent);
    espurnaRegisterReload(update);

    // the AP may be on already: the module starts after the Wi-Fi, or later from the MODULES page
    update();
}

} // namespace captive
} // namespace espurna

void captiveSetup() {
    espurna::captive::setup();
}

#endif
