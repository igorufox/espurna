/*

Part of the WEBSERVER module

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

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

namespace ota {
namespace web {
namespace {

STRING_VIEW_INLINE(Prefix, "ota");

void onVisible(JsonObject& root) {
    wsPayloadModule(root, Prefix);
}

void sendResponse(AsyncWebServerRequest *request, int code, const String& payload = "") {
    auto *response = request->beginResponseStream(F("text/plain"), 256);

    webSecurityHeaders(response);
    response->addHeader(
        STRING_VIEW("Connection").toString(),
        STRING_VIEW("close").toString());

    response->setCode(code);

    if (payload.length()) {
        response->print(payload);
    } else {
        if (!Update.hasError()) {
            response->print(F("OK"));
        } else {
#if defined(ARDUINO_ESP8266_RELEASE_2_3_0)
            Update.printError(reinterpret_cast<Stream&>(response));
#else
            Update.printError(*response);
#endif
        }
    }

    request->send(response);
}

void setStatus(AsyncWebServerRequest *request, int code, const String& payload = "") {
    sendResponse(request, code, payload);
    request->_tempObject = malloc(sizeof(bool));
}

// Last chunk of the file arrives before the body is fully parsed. When some of the upload never
// reached the HTTP parser, the request does not fail. Instead, contentLength() is quietly lowered
// to what did arrive and the request finishes as usual. Only commit the update after parsing is
// done, and only when the body is the size the client said it is
size_t pending_size { 0 };
bool pending { false };

// An upload went in and the board restarts into it (the reset is requested from the loop a moment later)
bool installed { false };

// Upload that started the running Update. When its connection is closed before the upload is
// finished (client gone, request timed out), nothing else ends the Update. It would then stay
// 'in progress' and every following upload fails with 400 until the reboot
AsyncWebServerRequest* owner { nullptr };

// the chip's part: whether it takes an upload (ref. upload_check() in ota_driver.h)
namespace driver = ::espurna::ota::driver;

void onDisconnect(AsyncWebServerRequest* request) {
    if (owner != request) {
        return;
    }

    owner = nullptr;
    if (Update.isRunning()) {
        pending = false;
        Update.end();
        eepromRotate(true);
        DEBUG_MSG_P(PSTR("[UPGRADE] Connection closed before the upload finished, aborted\n"));
    }
}

// The web server drops a connection after 3 s without received data (WebServer.cpp). While the image goes
// into the flash, the receive buffers can be full and the sender waits for its TCP retransmission timer,
// which grows past that: the upload stopped half-way (nginx answered 502 to the browser)
constexpr uint32_t UploadRxTimeout { 30 };

void uploadRxTimeout(AsyncWebServerRequest* request) {
    request->client()->setRxTimeout(UploadRxTimeout);
}

bool complete(AsyncWebServerRequest *request) {
    const auto& header = request->header(F("Content-Length"));
    return header.length()
        && (static_cast<size_t>(header.toInt()) == request->contentLength());
}

void onUpgrade(AsyncWebServerRequest *request) {
    // not in onFile(), holding up AsyncTCP for every uploaded chunk only slows the upload down
    ::espurna::system::AsyncGuard guard;
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    if (request->_tempObject) {
        return;
    }

    if (pending) {
        pending = false;
        owner = nullptr;

        if (!complete(request)) {
            DEBUG_MSG_P(PSTR("[UPGRADE] Incomplete upload, received %u of %s bytes\n"),
                request->contentLength(), request->header(F("Content-Length")).c_str());
            Update.end();
            eepromRotate(true);
            sendResponse(request, 400, F("ERROR: Incomplete upload"));
            return;
        }

        if (guard.locked()) {
            otaFinalize(pending_size, CustomResetReason::Ota, true);
        } else {
            // Update belongs to this upload and is finished right here, so that the response
            // reports the actual result. What otaFinalize() does after that (reset, EEPROM rotation,
            // debug output) belongs to loop() and happens there. Never the case on esp8266.
            const auto size = pending_size;
            const auto success = Update.isRunning() && Update.end(true);
            installed = success;
            systemRunInLoop([size, success]() {
                if (success) {
                    DEBUG_MSG_P(PSTR("[OTA] Success: %7u bytes\n"), size);
                    prepareReset(CustomResetReason::Ota);
                    return;
                }

                otaPrintError();
                eepromRotate(true);
            });
        }
    }

    sendResponse(request, 200);
}

void onFile(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    // One answer per request, not one per chunk: the server sends it only after the whole body, and a 401 built for
    // every chunk of an unauthenticated upload ran an ESP8266 out of heap (abort in AsyncBasicResponse, D390FC 11-07)
    if (!request->_tempObject && !webAuthenticate(request)) {
        request->requestAuthentication(systemHostname().c_str());
        request->_tempObject = malloc(sizeof(bool));
        return;
    }

    // We set this after we are done with the request
    // It is still possible to re-enter this callback even after connection is already closed
    // 1.15.0: TODO: see https://github.com/me-no-dev/ESPAsyncWebServer/pull/660
    // remote close or request sending some data before finishing parsing of the body will leak 1460 bytes
    // waiting a bit for upstream. looks more and more we need to fork the server
    if (request->_tempObject) {
        return;
    }

    if (!index) {
        pending = false;

        // TODO: stop network activity completely when handling Update through ArduinoOTA or `ota` command?
        if (Update.isRunning()) {
            setStatus(request, 400, F("ERROR: Upgrade in progress"));
            return;
        }

#if MODULES_SUPPORT
        // the update is written where the modules are, while their code runs
        if (modulesRunning()) {
            setStatus(request, 409, F("ERROR: modules installed, MODULES.OFF first"));
            return;
        }
#endif

        // Check that header is correct and there is more data before anything is written to the flash
        if (final || !len) {
            setStatus(request, 400, F("ERROR: Invalid request"));
            return;
        }

        // the chip's word on this file (Beken: the bootloader it is for), before anything is written
        const auto* refused = driver::upload_check(data, len);
        if (refused) {
            setStatus(request, 400, String(FPSTR(refused)));
            return;
        }
    }

    if (!index) {
        if (!otaVerifyHeader(data, len)) {
            setStatus(request, 400, F("ERROR: No magic byte / invalid flash config"));
            return;
        }

        // Disabling EEPROM rotation to prevent writing to EEPROM after the upgrade
        eepromRotate(false);

        DEBUG_MSG_P(PSTR("[UPGRADE] Start: %s\n"), filename.c_str());
        driver::update_prepare();

        // Note: cannot use request->contentLength() for multipart/form-data
        const size_t size = driver::update_size();
        if (!Update.begin(size)) {
            setStatus(request, 500);
            eepromRotate(true);
            return;
        }

        owner = request;
        uploadRxTimeout(request);
        request->onDisconnect([request]() {
            onDisconnect(request);
        });
    }

    if (request->_tempObject) {
        return;
    }

    // Any error will cancel the update, but request may still be alive
    if (!Update.isRunning()) {
        return;
    }

    if (Update.write(data, len) != len) {
        setStatus(request, 500);
        Update.end();
        eepromRotate(true);
        return;
    }

    if (final) {
        // committed in onUpgrade(), see above
        pending_size = index + len;
        pending = true;
    } else {
        otaProgress(index + len);
    }
}

#if defined(ARDUINO_ARCH_ESP8266)
// Install from GitHub (fwinstall.mjs): the board downloads the file itself, ota_fetch.cpp
bool authenticated(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        request->requestAuthentication(systemHostname().c_str());
        return false;
    }

    return true;
}

void onFetchStatus(AsyncWebServerRequest* request) {
    if (authenticated(request)) {
        request->send(200, "application/json", otaFetchStatus());
    }
}

String formValue(AsyncWebServerRequest* request, const __FlashStringHelper* name) {
    const auto* param = request->getParam(String(name), true);
    return param ? param->value() : String();
}

// url + sha256 (of firmware.json). "OK, fetch" - restarting, this image downloads it at boot; "OK, mini" - stored
// for the minimal image (1 MB boards), the browser uploads espurna-esp8266-1m-mini.bin.gz to /upgrade next
void onFetch(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    if (Update.isRunning()) {
        sendResponse(request, 409, F("ERROR: Upgrade in progress"));
        return;
    }

    String error;
    if (!otaFetchRequest(formValue(request, F("url")), formValue(request, F("sha256")), error)) {
        sendResponse(request, 400, String(F("ERROR: ")) + error);
        return;
    }

    if (otaFetchSelf()) {
        sendResponse(request, 200, F("OK, fetch"));
        prepareReset(CustomResetReason::Web);
        return;
    }

    sendResponse(request, 200, F("OK, mini"));
}
#endif

} // namespace
} // namespace web
} // namespace ota

bool otaWebUploading() {
    return (ota::web::owner != nullptr) || ota::web::pending || ota::web::installed;
}

void otaWebSetup() {
#if defined(ARDUINO_ARCH_ESP8266)
    webServer().on("/ota/status", HTTP_GET, ota::web::onFetchStatus);
    webServer().on("/ota/fetch", HTTP_POST, ota::web::onFetch);
#endif
    webServer().
        on("/upgrade", HTTP_POST, ota::web::onUpgrade, ota::web::onFile);
    wsRegister().
        onVisible(ota::web::onVisible);
}

#endif // OTA_WEB_SUPPORT


