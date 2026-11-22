/*

Part of the WEBSERVER MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

/upgrade (an image uploaded by the browser, the file of a form or the raw body; the browser also downloads the releases,
fwinstall.mjs). On the one HTTP server of ESPurna (libs/http_server.h): the body comes a piece at a time into
the one image receiver (libs/image_receiver.h) and the head's target (ota.cpp), the handlers run in the loop.

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

void onVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, Prefix);
}

void sendResponse(WebRequest& request, int code, const espurna::string& payload) {
    webSecurityHeaders(request);
    webSend(request, code, "text/plain", payload);
}

// The upload of one request, its context (freed with it, an image not finished by then is aborted): the one image
// receiver (libs/image_receiver.h) into the head's target (ota.cpp)
struct Upload {
    Upload() :
        receiver(otaUpdateTarget())
    {}

    espurna::image::Receiver receiver;
    int code { 0 };         // the answer of a refused upload, 0 - fine so far
    espurna::string error;
};

// An upload went in and the board restarts into it (the reset is requested from the loop a moment later)
bool installed { false };

Upload* uploadOf(WebRequest& request) {
    auto* upload = static_cast<Upload*>(request.context());
    if (!upload) {
        upload = new (std::nothrow) Upload();
        request.setContext(upload, [](void* ptr) {
            auto* upload = static_cast<Upload*>(ptr);
            if (!upload->receiver.done() && upload->receiver.received()) {
                DEBUG_MSG_P(PSTR("[UPGRADE] Connection closed before the upload finished, aborted\n"));
            }
            delete upload;
        });
    }
    return upload;
}

void refuse(Upload& upload, int code, const espurna::string& error) {
    upload.code = code;
    upload.error = error;
}

// a page of another site (8.6), ref. espurna::image::sameOrigin()
bool sameOrigin(WebRequest& request) {
    return espurna::image::sameOrigin(
        request.header("Origin"), request.header("Host"), request.header("X-Forwarded-Host"));
}

// a piece of the image: the first one decides, every one goes into the receiver
void onFile(WebRequest& request, const char* filename, size_t index, const uint8_t* data, size_t len, bool) {
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
            refuse(*upload, 403, "ERROR: Origin");
            return;
        }

#if MODULES_SUPPORT
        // the update is written where the modules are, while their code runs
        if (modulesRunning()) {
            refuse(*upload, 409, "ERROR: modules installed, MODULES.OFF first");
            return;
        }
#endif

        DEBUG_MSG_P(PSTR("[UPGRADE] Start: %s, %u bytes\n"),
            (filename && filename[0]) ? filename : "(body)", static_cast<unsigned>(request.fileSize()));
        upload->receiver.expect(request.fileSize());
    }

    const char* error = upload->receiver.push(data, len);
    if (error) {
        refuse(*upload, 400, espurna::string("ERROR: ") + error);
    }
}

void onUpgrade(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    auto* upload = uploadOf(request);
    if (!upload) {
        sendResponse(request, 500, "ERROR: out of memory");
        return;
    }

    if (upload->code) {
        sendResponse(request, upload->code, upload->error.length() ? upload->error : espurna::string("ERROR"));
        return;
    }

    const char* error = upload->receiver.finish();
    if (error) {
        DEBUG_MSG_P(PSTR("[UPGRADE] %s\n"), error);
        sendResponse(request, 400, espurna::string("ERROR: ") + error);
        return;
    }

    installed = true;
    prepareReset(CustomResetReason::Ota);
    sendResponse(request, 200, "OK");
}

} // namespace
} // namespace web
} // namespace ota

bool otaWebUploading() {
    return otaRunning() || ota::web::installed;
}

void otaWebSetup() {
    auto& server = webServer();
    server.on("/upgrade", WebMethod::Post, ota::web::onUpgrade, ota::web::onFile);
    // the chip's own routes (ESP8266: /upgrade/bootloader)
    espurna::ota::driver::web_setup();
    wsRegister()
        .onVisible(ota::web::onVisible);
}

#endif // OTA_WEB_SUPPORT
