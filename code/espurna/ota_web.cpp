/*

Part of the WEBSERVER MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

/upgrade (an image uploaded by the browser, the file of a form or the raw body), /ota/fetch and /ota/status (the update by
an URL, ota_fetch.cpp). On the one HTTP server of ESPurna (libs/http_server.h): the body comes a piece at a time into
Update, the handlers run in the loop.

*/

#include "espurna.h"

#if WEB_SUPPORT && OTA_WEB_SUPPORT

#include "espurna_ota.h"
#include "ota_driver.h"
#include "web.h"

#if MODULES_SUPPORT
#include "modules.h"
#endif
#include "ws.h"

#include "libs/PrintString.h"

namespace ota {
namespace web {
namespace {

STRING_VIEW_INLINE(Prefix, "ota");

void onVisible(JsonObject& root) {
    wsPayloadModule(root, Prefix);
}

String updateError() {
    PrintString out(64);
    Update.printError(out);
    String result(out.c_str());
    result.trim();
    return result;
}

void sendResponse(WebRequest& request, int code, const String& payload) {
    webSecurityHeaders(request);
    webSend(request, code, "text/plain", payload);
}

// the chip's part: whether it takes an upload (ref. upload_check() in ota_driver.h)
namespace driver = ::espurna::ota::driver;

// The upload of one request, its context: freed with it
struct Upload {
    int code { 0 };         // the answer of a refused or failed upload, 0 - fine so far
    String error;
    bool running { false }; // this request's Update
    size_t size { 0 };
    bool complete { false };
};

// An upload went in and the board restarts into it (the reset is requested from the loop a moment later)
bool installed { false };

// The request whose Update runs: one at a time
Upload* running { nullptr };

void abort(Upload& upload) {
    if (upload.running) {
        upload.running = false;
        running = nullptr;
        Update.end();
        eepromRotate(true);
    }
}

Upload* uploadOf(WebRequest& request) {
    auto* upload = static_cast<Upload*>(request.context());
    if (!upload) {
        upload = new (std::nothrow) Upload();
        request.setContext(upload, [](void* ptr) {
            auto* upload = static_cast<Upload*>(ptr);
            // gone before the end (the client went away): nothing else ends the Update
            if (upload->running) {
                DEBUG_MSG_P(PSTR("[UPGRADE] Connection closed before the upload finished, aborted\n"));
                abort(*upload);
            }
            delete upload;
        });
    }
    return upload;
}

void fail(Upload& upload, int code, const String& error) {
    abort(upload);
    upload.code = code;
    upload.error = error;
}

// a page of another site (8.6), ref. otaWebSameOrigin()
bool sameOrigin(WebRequest& request) {
    return otaWebSameOrigin(
        webHeader(request, "Origin"),
        webHeader(request, "Host"),
        webHeader(request, "X-Forwarded-Host"));
}

// a piece of the image: the first one decides, every one goes into Update
void onFile(WebRequest& request, const char* filename, size_t index, const uint8_t* data, size_t len, bool final) {
    auto* upload = uploadOf(request);
    if (!upload || upload->code) {
        return;
    }

    if (!index) {
        // nothing is written for a client without the login or for a page of another site
        if (!webAuthenticate(request)) {
            upload->code = 401;
            return;
        }

        if (!sameOrigin(request)) {
            DEBUG_MSG_P(PSTR("[UPGRADE] Refused, Origin %s is not this board\n"), webHeader(request, "Origin").c_str());
            fail(*upload, 403, F("ERROR: Origin"));
            return;
        }

        if (running || Update.isRunning()) {
            fail(*upload, 400, F("ERROR: Upgrade in progress"));
            return;
        }

#if MODULES_SUPPORT
        // the update is written where the modules are, while their code runs
        if (modulesRunning()) {
            fail(*upload, 409, F("ERROR: modules installed, MODULES.OFF first"));
            return;
        }
#endif

        if (!len) {
            fail(*upload, 400, F("ERROR: Invalid request"));
            return;
        }

        // the chip's word on this file (Beken: the bootloader it is for), before anything is written
        const auto* refused = driver::upload_check(data, len);
        if (refused) {
            fail(*upload, 400, String(FPSTR(refused)));
            return;
        }

        if (!otaVerifyHeader(const_cast<uint8_t*>(data), len)) {
            fail(*upload, 400, F("ERROR: No magic byte / invalid flash config"));
            return;
        }

        // Disabling EEPROM rotation to prevent writing to EEPROM after the upgrade
        eepromRotate(false);

        DEBUG_MSG_P(PSTR("[UPGRADE] Start: %s\n"), (filename && filename[0]) ? filename : "(body)");
        driver::update_prepare();

        // the size of a form part is not known in advance
        if (!Update.begin(driver::update_size())) {
            eepromRotate(true);
            fail(*upload, 500, updateError());
            return;
        }

        upload->running = true;
        running = upload;
    }

    if (!upload->running) {
        return;
    }

    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
        fail(*upload, 500, updateError());
        return;
    }

    upload->size = index + len;
    if (final) {
        upload->complete = true;
    } else {
        otaProgress(index + len);
    }
}

void onUpgrade(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    auto* upload = uploadOf(request);
    if (!upload) {
        sendResponse(request, 500, F("ERROR: out of memory"));
        return;
    }

    if (upload->code) {
        sendResponse(request, upload->code, upload->error.length() ? upload->error : String(F("ERROR")));
        return;
    }

    if (!upload->running || !upload->complete) {
        DEBUG_MSG_P(PSTR("[UPGRADE] Incomplete upload, received %u of %u bytes\n"),
            request.bodyReceived(), request.contentLength());
        abort(*upload);
        sendResponse(request, 400, F("ERROR: Incomplete upload"));
        return;
    }

    upload->running = false;
    running = nullptr;
    installed = otaFinalize(upload->size, CustomResetReason::Ota, true);
    sendResponse(request, installed ? 200 : 500, installed ? String(F("OK")) : updateError());
}

// Install from GitHub (fwinstall.mjs): the board downloads the file itself, ota_fetch.cpp
bool authenticated(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return false;
    }

    return true;
}

void onFetchStatus(WebRequest& request) {
    if (authenticated(request)) {
        webSend(request, 200, "application/json", otaFetchStatus());
    }
}

// url + sha256 (of firmware.json). "OK, fetch" - restarting: the next boot (ESP8266, Beken) or safeboot (ESP32)
// downloads it, ota_fetch.cpp
void onFetch(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    if (!sameOrigin(request)) {
        sendResponse(request, 403, F("ERROR: Origin"));
        return;
    }

    if (running || Update.isRunning()) {
        sendResponse(request, 409, F("ERROR: Upgrade in progress"));
        return;
    }

    String error;
    if (!otaFetchRequest(webParam(request, "url"), webParam(request, "sha256"), error)) {
        sendResponse(request, 400, String(F("ERROR: ")) + error);
        return;
    }

    sendResponse(request, 200, F("OK, fetch"));
    prepareReset(CustomResetReason::Web);
}

} // namespace
} // namespace web
} // namespace ota

bool otaWebUploading() {
    return (ota::web::running != nullptr) || ota::web::installed;
}

void otaWebSetup() {
    auto& server = webServer();
    server.on("/ota/status", WebMethod::Get, ota::web::onFetchStatus);
    server.on("/ota/fetch", WebMethod::Post, ota::web::onFetch);
    server.on("/upgrade", WebMethod::Post, ota::web::onUpgrade, ota::web::onFile);
    wsRegister()
        .onVisible(ota::web::onVisible);
}

#endif // OTA_WEB_SUPPORT
