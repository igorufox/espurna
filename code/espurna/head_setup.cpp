/*

HEAD SETUP: the admin password and the Wi-Fi network of a fresh board without the WebUI module (W71). The form is on the
head page (modules_page.h); the default modules (config/head.h) have no webui/ws, and over the board's own access point
there is no internet for "From GitHub". Every chip, in the head.

GET  /setup        {"pass":true|false,"ssid":"<ssid0>"} - pass: the default password is still on (systemPasswordMustChange,
                   the WebUI's rule), a new one is required before anything else is saved
GET  /setup/scan   {"run":..,"error":"..","list":[["ssid",rssi,open],..]}; ?start=1 starts a scan (the station has to be on)
POST /setup        pass (adminPass), ssid + wpass (ssid0, pass0): saved, then a restart

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT

#include "espurna_wifi.h"
#include "system.h"
#include "web.h"
#include "libs/image_receiver.h"
#include "libs/text.h"

#include <cstring>

namespace espurna {
namespace head_setup {
namespace {

// the login, as every head route (modules.cpp authenticated())
bool authenticated(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return false;
    }

    return true;
}

// a POST that changes the board: the login and a page of this board (modules.cpp allowed())
bool allowed(WebRequest& request) {
    if (!authenticated(request)) {
        return false;
    }

    if (!espurna::image::sameOrigin(
        request.header("Origin"), request.header("Host"), request.header("X-Forwarded-Host")))
    {
        request.send(403, "text/plain", "ERROR: Origin");
        return false;
    }

    return true;
}

void json(WebRequest& request, espurna::string&& out) {
    webSend(request, 200, "application/json", std::move(out));
}

// --- scan ----------------------------------------------------------------------------------------------------------

// what the last scan found, JSON array items; kept until a reply after its end took it
espurna::string scan_list;
espurna::string scan_error;

// a few dozen networks at most; the rest is dropped (RAM of the head)
constexpr size_t ScanListMax { 1536 };

void scanFound(const espurna::string& ssid, int8_t rssi, bool open) {
    if (!ssid.length() || (scan_list.length() > ScanListMax)) {
        return;
    }

    if (scan_list.length()) {
        scan_list += ',';
    }

    scan_list += '[';
    text::jsonString(scan_list, ssid.c_str(), ssid.length());
    scan_list += ',';
    scan_list += espurna::string(static_cast<int>(rssi));
    scan_list += open ? ",1]" : ",0]";
}

void scanError(const espurna::string& error) {
    scan_error = error;
}

void onScan(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    if (request.hasParam("start") && !wifiScanning()) {
        scan_list = espurna::string();
        scan_error = espurna::string();
        wifiScanStart(scanFound, scanError);
    }

    const bool running = wifiScanning();

    espurna::string out;
    out.reserve(32 + scan_list.length() + scan_error.length());
    out += "{\"run\":";
    out += running ? '1' : '0';
    out += ",\"error\":";
    text::jsonString(out, scan_error.c_str(), scan_error.length());
    out += ",\"list\":[";
    out += scan_list;
    out += "]}";

    if (!running) {
        scan_list = espurna::string();
        scan_error = espurna::string();
    }

    json(request, std::move(out));
}

// --- the form ------------------------------------------------------------------------------------------------------

void onState(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    const auto ssid = getSetting(settings::Key("ssid", 0));

    espurna::string out;
    out.reserve(32 + ssid.length());
    out += "{\"pass\":";
    out += systemPasswordMustChange() ? "true" : "false";
    out += ",\"ssid\":";
    text::jsonString(out, ssid.c_str(), ssid.length());
    out += '}';

    json(request, std::move(out));
}

// The admin password is also the one of the board's access point (WPA2: 8..63 printable ASCII, config/general.h) and
// must differ from the current one (system.cpp onKeyCheck). The page checks the WebUI's policy (password/utils.mjs)
bool validPassword(const char* value) {
    const size_t len = strlen(value);
    if ((len < 8) || (len > 63)) {
        return false;
    }

    for (size_t index = 0; index < len; ++index) {
        if ((value[index] < 0x20) || (value[index] > 0x7e)) {
            return false;
        }
    }

    return !systemPasswordEquals(StringView(value, len));
}

void refuse(WebRequest& request, const char* text) {
    webSend(request, 400, "text/plain", espurna::string(text));
}

void onSave(WebRequest& request) {
    if (!allowed(request)) {
        return;
    }

    const char* pass = request.param("pass");
    const char* ssid = request.param("ssid");
    const char* wpass = request.param("wpass");

    const bool new_pass = pass && *pass;
    const bool new_wifi = ssid && *ssid;

    if (!new_pass && systemPasswordMustChange()) {
        refuse(request, "ERROR: the default password is still on, set a new one first");
        return;
    }

    if (new_pass && !validPassword(pass)) {
        refuse(request, "ERROR: the password must be 8..63 printable characters, not the current one");
        return;
    }

    const size_t wpass_len = wpass ? strlen(wpass) : 0;
    if (new_wifi && ((strlen(ssid) > 32) || (wpass_len && ((wpass_len < 8) || (wpass_len > 64))))) {
        refuse(request, "ERROR: the network name is 1..32 bytes, its password empty or 8..64 characters");
        return;
    }

    if (!new_pass && !new_wifi) {
        refuse(request, "ERROR: nothing to save");
        return;
    }

    if (new_pass) {
        setSetting(settings::Key("adminPass"), espurna::string(pass));
    }

    if (new_wifi) {
        setSetting(settings::Key("ssid", 0), espurna::string(ssid));
        setSetting(settings::Key("pass", 0), espurna::string(wpass ? wpass : ""));
        // a pinned access point of the network before (wifi.cpp bssid / chan) would keep the board off the new one
        delSetting(settings::Key("bssid", 0));
        delSetting(settings::Key("chan", 0));
    }

    saveSettings();
    webSend(request, 200, "text/plain", espurna::string("OK"));
    prepareReset(CustomResetReason::Web);
}

} // namespace
} // namespace head_setup
} // namespace espurna

void headSetupRoutes() {
    using namespace espurna::head_setup;
    auto& server = webServer();
    server.on("/setup", WebMethod::Get, onState);
    server.on("/setup", WebMethod::Post, onSave);
    server.on("/setup/scan", WebMethod::Get, onScan);
}

#endif // MODULES_SUPPORT
