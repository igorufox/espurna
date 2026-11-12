/*

NOFUSS MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

The NoFUSS protocol (https://github.com/xoseperez/nofuss) on the one HTTP client of ESPurna (libs/http_client.h), every
chip alike: a GET of `nofussServer` with the board in the X-ESP8266-* headers, a JSON answer with "version" and
"firmware" when there is something newer, then the update by its URL through the head's OTA (otaFetchRequest(): the
same download, checks and restart as /ota/fetch). The old NoFUSS library and its ESP8266HTTPClient / ESPhttpUpdate are
gone with the other clients. No file system update: there is no file system (the "fs" / "spiffs" of the answer is
reported and left)

*/

#include "espurna.h"

#if NOFUSS_SUPPORT

#include "espurna_ota.h"
#include "espurna_wifi.h"
#include "nofuss.h"
#include "terminal.h"
#include "ws.h"

#include "libs/http_client.h"

#include <ArduinoJson.h>

namespace {

unsigned long _nofussLastCheck = 0;
unsigned long _nofussInterval = 0;
bool _nofussEnabled = false;

String _nofussServer;
String _nofussHeaders;

// the answer of the server: a small JSON
struct AnswerSink : public espurna::http::Sink {
    String text;

    const char* begin(const espurna::http::Head& head) override {
        text = String();
        return (head.status == 200) ? nullptr : PSTR("no update answer");
    }

    const char* write(const uint8_t* data, size_t len) override {
        if ((text.length() + len) > 1024) {
            return PSTR("answer too long");
        }
        text.concat(reinterpret_cast<const char*>(data), len);
        return nullptr;
    }

    const char* end() override {
        return nullptr;
    }
};

espurna::http::Client _nofussClient;
AnswerSink _nofussAnswer;

// an absolute URL as is, a path from the server
String _nofussUrl(const String& path) {
    if (path.startsWith(F("http"))) {
        return path;
    }

    String out = _nofussServer;
    const int scheme = out.indexOf(F("://"));
    const int slash = out.indexOf('/', (scheme >= 0) ? (scheme + 3) : 0);
    if (slash >= 0) {
        out = out.substring(0, slash);
    }
    if (!path.startsWith(F("/"))) {
        out += '/';
    }
    out += path;
    return out;
}

void _nofussDone(const espurna::http::Result& result) {
    if (!result.ok) {
        DEBUG_MSG_P(PSTR("[NOFUSS] Wrong server response: %d %s\n"), result.status, result.message);
        return;
    }

    DynamicJsonBuffer buffer(512);
    JsonObject& answer = buffer.parseObject(_nofussAnswer.text.c_str());
    if (!answer.success()) {
        DEBUG_MSG_P(PSTR("[NOFUSS] Error parsing server response\n"));
        return;
    }

    const String version = answer["version"] | "";
    const String firmware = answer["firmware"] | "";
    if (!version.length() || !firmware.length()) {
        DEBUG_MSG_P(PSTR("[NOFUSS] Already in the last version\n"));
        return;
    }

    const String fs = answer.containsKey("fs") ? String(answer["fs"] | "") : String(answer["spiffs"] | "");
    if (fs.length()) {
        DEBUG_MSG_P(PSTR("[NOFUSS] File system image %s left out: no file system\n"), fs.c_str());
    }

    const auto url = _nofussUrl(firmware);
    DEBUG_MSG_P(PSTR("[NOFUSS] Updating to %s: %s\n"), version.c_str(), url.c_str());

    String error;
    if (!otaFetchRequest(url, String(), error)) {
        DEBUG_MSG_P(PSTR("[NOFUSS] Update Error: %s\n"), error.c_str());
    }
}

#if WEB_SUPPORT

STRING_VIEW_INLINE(NofussPrefix, "nofuss");

bool _nofussWebSocketOnKeyCheck(espurna::StringView key, const JsonVariant&) {
    return key.startsWith(NofussPrefix);
}

void _nofussWebSocketOnVisible(JsonObject& root) {
    wsPayloadModule(root, NofussPrefix);
}

void _nofussWebSocketOnConnected(JsonObject& root) {
    root["nofussEnabled"] = getSetting("nofussEnabled", 1 == NOFUSS_ENABLED);
    root["nofussServer"] = getSetting("nofussServer", NOFUSS_SERVER);
}

#endif

void _nofussConfigure() {
    _nofussServer = getSetting("nofussServer", NOFUSS_SERVER);
    if (!_nofussServer.length()) {
        setSetting("nofussEnabled", 0);
        _nofussEnabled = false;
    } else {
        _nofussEnabled = getSetting("nofussEnabled", 1 == NOFUSS_ENABLED);
    }
    _nofussInterval = getSetting("nofussInterval", NOFUSS_INTERVAL);
    _nofussLastCheck = 0;

    if (!_nofussEnabled) {
        DEBUG_MSG_P(PSTR("[NOFUSS] Disabled\n"));
        return;
    }

    const auto info = buildInfo();
    String device;
    device += String(info.app.name);
    device += '_';
    device += String(info.hardware.device);

    const auto version = String(info.app.version);
    const auto time = String(buildTime());

    // what the NoFUSS server knows a board by
    _nofussHeaders = String();
    _nofussHeaders += F("X-ESP8266-MAC: ");
    _nofussHeaders += WiFi.macAddress();
    _nofussHeaders += F("\r\nX-ESP8266-DEVICE: ");
    _nofussHeaders += device;
    _nofussHeaders += F("\r\nX-ESP8266-VERSION: ");
    _nofussHeaders += version;
    _nofussHeaders += F("\r\nX-ESP8266-BUILD: ");
    _nofussHeaders += time;
    _nofussHeaders += F("\r\nX-ESP8266-CHIPID: ");
    _nofussHeaders += systemChipId().toString();
    _nofussHeaders += F("\r\nX-ESP8266-CHIPSIZE: ");
    _nofussHeaders += String(ESP.getFlashChipRealSize());
    _nofussHeaders += F("\r\n");

    DEBUG_MSG_P(PSTR("[NOFUSS] Server: %s\n"), _nofussServer.c_str());
    DEBUG_MSG_P(PSTR("[NOFUSS] Device: %s\n"), device.c_str());
    DEBUG_MSG_P(PSTR("[NOFUSS] Version: %s\n"), version.c_str());
    DEBUG_MSG_P(PSTR("[NOFUSS] Build: %s\n"), time.c_str());
}

void _nofussLoop() {
    _nofussClient.loop();

    if (!_nofussEnabled || !wifiConnected() || _nofussClient.busy()) {
        return;
    }

    if ((_nofussLastCheck > 0) && ((millis() - _nofussLastCheck) < _nofussInterval)) {
        return;
    }

    nofussRun();
}

#if TERMINAL_SUPPORT
PROGMEM_STRING(NofussCommand, "NOFUSS");

void _nofussCommand(::terminal::CommandContext&& ctx) {
    terminalOK(ctx);
    nofussRun();
}

static constexpr ::terminal::Command NofussCommands[] PROGMEM {
    {NofussCommand, _nofussCommand},
};
#endif // TERMINAL_SUPPORT

} // namespace

void nofussRun() {
    _nofussLastCheck = millis();
    if (_nofussClient.busy() || !_nofussServer.length()) {
        return;
    }

    DEBUG_MSG_P(PSTR("[NOFUSS] Start\n"));
    espurna::http::Request request;
    request.url = _nofussServer.c_str();
    request.headers = _nofussHeaders.c_str();
    if (!_nofussClient.start(request, _nofussAnswer, _nofussDone)) {
        DEBUG_MSG_P(PSTR("[NOFUSS] Could not start the request\n"));
    }
}

void nofussSetup() {
    _nofussConfigure();

#if WEB_SUPPORT
    wsRegister()
        .onVisible(_nofussWebSocketOnVisible)
        .onConnected(_nofussWebSocketOnConnected)
        .onKeyCheck(_nofussWebSocketOnKeyCheck);
#endif

#if TERMINAL_SUPPORT
    espurna::terminal::add(NofussCommands);
#endif

    espurnaRegisterLoop(_nofussLoop);
    espurnaRegisterReload(_nofussConfigure);
}

#endif // NOFUSS_SUPPORT
