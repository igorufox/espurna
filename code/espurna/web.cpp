/*

WEBSERVER MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

The one HTTP server of ESPurna (libs/http_server.h on lwIP, every chip). Its handlers run in the loop, so the settings,
the modules, the OTA state are read and changed there directly: no lock to wait for, no "busy" answer.

*/

#include "espurna.h"

#if WEB_SUPPORT

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>

#include <ArduinoJson.h>

#include "ntp.h"
#include "settings.h"
#include "system.h"
#include "utils.h"
#include "web.h"
#include "ws.h"
#include "web_guard.h"

#if MODULES_SUPPORT
#include "modules.h"
#endif

namespace espurna {
namespace web {
namespace {

namespace build {

STRING_VIEW_INLINE(WebRemoteDomain, WEB_REMOTE_DOMAIN);

constexpr uint16_t port() {
    return (WEB_PORT == 0) ? 80 : (WEB_PORT);
}

constexpr bool access_log() {
    return 1 == WEB_ACCESS_LOG;
}

} // namespace build

namespace settings {
namespace keys {

STRING_VIEW_INLINE(Prefix, "web");

STRING_VIEW_INLINE(AccessLog, "webAccessLog");
STRING_VIEW_INLINE(Domain, "webDomain");
STRING_VIEW_INLINE(Port, "webPort");

// own device list of the WebUI device template search, the default one when empty
STRING_VIEW_INLINE(TemplatesUrl, "webTplUrl");

// firmware.json of the WebUI OTA install from GitHub, the default one when empty
STRING_VIEW_INLINE(FirmwareUrl, "webFwUrl");

} // namespace keys

bool access_log() {
    return getSetting(keys::AccessLog, build::access_log());
}

String domain() {
    return getSetting(keys::Domain, build::WebRemoteDomain);
}

uint16_t port() {
    return getSetting(keys::Port, build::port());
}

} // namespace settings

constexpr size_t ConfigBufferMax { 4096 };

uint16_t port {};
WebServer server;
bool log_requests { false };

std::vector<web_request_callback_f> request_callbacks;
std::vector<web_body_callback_f> body_callbacks;

// the settings restore (POST /config): the body, kept until it is complete
struct ConfigUpload {
    std::vector<uint8_t> data;
    bool failed { false };
};

void addSecurityHeaders(WebRequest& request) {
    request.addHeader("X-XSS-Protection", "1; mode=block");
    request.addHeader("X-Content-Type-Options", "nosniff");
    request.addHeader("X-Frame-Options", "deny");
}

bool authenticate(WebRequest& request) {
#if USE_PASSWORD
    return request.authenticate(WEB_USERNAME, systemPassword().c_str(), systemHostname().c_str());
#else
    (void)request;
    return true;
#endif
}

void requestAuthentication(WebRequest& request) {
    request.requestAuthentication(systemHostname().c_str());
}

// Whether the request came to the access point by its address or the hostname
bool apModeRequest(WebRequest& request) {
    if (!wifiConnectable()) {
        return false;
    }

    if (request.localIp() != static_cast<uint32_t>(wifiApIp())) {
        return false;
    }

    const char* host = request.header("Host");
    if (!host) {
        return false;
    }

    const String value(host);
    return value.equals(ipToString(wifiApIp())) || value.startsWith(systemHostname() + '.');
}

// in the AP mode, only requests that use our hostname or IP
bool allowed(WebRequest& request) {
    if (wifiConnectable() && !apModeRequest(request)) {
        request.send(404, "text/plain", "Not Found");
        return false;
    }

    return true;
}

void onReset(WebRequest& request) {
    if (!authenticate(request)) {
        requestAuthentication(request);
        return;
    }

    prepareReset(CustomResetReason::Web);
    request.send(200, "text/plain", "OK");
}

void onDiscover(WebRequest& request) {
    StaticJsonBuffer<JSON_OBJECT_SIZE(5) + 128> buffer;
    JsonObject& root = buffer.createObject();

    root["hostname"] = systemHostname();
    root["device"] = systemDevice();

    const auto app = buildApp();
    root["app"] = app.name;
    root["version"] = app.version;

    String out;
    root.printTo(out);
    request.send(200, "application/json", out.c_str());
}

// the whole settings storage as a JSON file
void onGetConfig(WebRequest& request) {
    if (!authenticate(request)) {
        requestAuthentication(request);
        return;
    }

    auto out = std::make_shared<String>();
    out->reserve(1024);

    const auto app = buildApp();

    char buffer[256];
    int prefix_len = snprintf_P(buffer, sizeof(buffer),
            PSTR("{\n\"app\": \"%s\",\n\"version\": \"%s\",\n\"backup\": \"1\""),
            app.name.c_str(), app.version.c_str());
    if (prefix_len <= 0) {
        request.send(500, "text/plain", "");
        return;
    }
    out->concat(buffer, prefix_len);

    espurna::settings::foreach([&](espurna::settings::kvs_type::KeyValueResult&& kv) {
        auto key = kv.key.toString();
        auto value = kv.value.toString();

        int len = snprintf_P(buffer, sizeof(buffer), PSTR("\"%s\": \"%s\""), key.c_str(), value.c_str());
        if (len > 0) {
            *out += ",\n";
            out->concat(buffer, len);
        }
    });
    *out += "\n}";

    auto get_timestamp = []() -> String {
#if NTP_SUPPORT
        if (ntpSynced()) {
            return ntpDateTime();
        }
#endif
        return String(espurna::time::millis().time_since_epoch().count(), 10);
    };

    int written = snprintf_P(buffer, sizeof(buffer),
        PSTR("attachment; filename=\"%s %s backup.json\""),
        systemHostname().c_str(), get_timestamp().c_str());
    if (written <= 0) {
        request.send(500, "text/plain", "");
        return;
    }

    addSecurityHeaders(request);
    request.addHeader("Content-Disposition", buffer);
    request.send(200, "application/json", static_cast<long>(out->length()),
        [out](uint8_t* data, size_t max, size_t index) -> size_t {
            const size_t len = out->length();
            const size_t have = (index < len) ? std::min(len - index, max) : 0;
            if (have) {
                std::copy(out->c_str() + index, out->c_str() + index + have, data);
            }
            return have;
        });
}

ConfigUpload* configUpload(WebRequest& request) {
    auto* upload = static_cast<ConfigUpload*>(request.context());
    if (!upload) {
        upload = new (std::nothrow) ConfigUpload();
        request.setContext(upload, [](void* ptr) {
            delete static_cast<ConfigUpload*>(ptr);
        });
    }
    return upload;
}

// the restore file, as the raw body or the file part of a form
void onPostConfigBody(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    auto* upload = configUpload(request);
    if (!upload || upload->failed) {
        return;
    }

    if (!index) {
        upload->data.clear();
    }

    if ((upload->data.size() + len) > std::min(ConfigBufferMax, systemFreeHeap() / 2)) {
        upload->failed = true;
        upload->data.clear();
        return;
    }

    upload->data.insert(upload->data.end(), data, data + len);
}

void onPostConfig(WebRequest& request) {
    if (!authenticate(request)) {
        requestAuthentication(request);
        return;
    }

    auto* upload = static_cast<ConfigUpload*>(request.context());
    bool success = false;
    if (upload && !upload->failed && !upload->data.empty()) {
        upload->data.push_back(0);
        success = settingsRestoreJson(reinterpret_cast<char*>(upload->data.data()));
    }

    request.send(success ? 200 : 400, "text/plain", success ? "OK" : "Error");
}

// every request no route took: the subscribers (alexa, prometheus, terminal), else 404
void onRequest(WebRequest& request) {
    if (!allowed(request)) {
        return;
    }

    for (auto& callback : request_callbacks) {
        if (callback(request)) {
            return;
        }
    }

    if (!request.answered()) {
        request.send(404, "text/plain", "Not Found");
    }
}

void onBody(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    if (wifiConnectable() && !apModeRequest(request)) {
        return;
    }

    for (auto& callback : body_callbacks) {
        if (callback(request, data, len, index, request.contentLength())) {
            return;
        }
    }
}

void onVisible(JsonObject& root) {
    root[settings::keys::Port] = port;
    root[settings::keys::TemplatesUrl] = getSetting(settings::keys::TemplatesUrl);
    root[settings::keys::FirmwareUrl] = getSetting(settings::keys::FirmwareUrl);
}

bool onKeyCheck(espurna::StringView key, const JsonVariant&) {
    return key.startsWith(settings::keys::Prefix);
}

void setupAccessControlHeaders() {
    const auto domain = settings::domain();
    server.defaultHeader("Access-Control-Allow-Origin", domain.c_str());
    if (!domain.equals("*")) {
        server.defaultHeader("Access-Control-Allow-Credentials", "true");
    }
}

void loop() {
    server.loop();
}

} // namespace
} // namespace web
} // namespace espurna

void webSecurityHeaders(WebRequest& request) {
    espurna::web::addSecurityHeaders(request);
}

bool webApModeRequest(WebRequest& request) {
    return espurna::web::apModeRequest(request);
}

bool webAuthenticate(WebRequest& request) {
    return espurna::web::authenticate(request);
}

void webRequestAuthentication(WebRequest& request) {
    espurna::web::requestAuthentication(request);
}

String webHeader(WebRequest& request, const char* name) {
    const char* value = request.header(name);
    return value ? String(value) : String();
}

String webParam(WebRequest& request, const char* name) {
    const char* value = request.param(name);
    return value ? String(value) : String();
}

void webSend(WebRequest& request, int code, const char* type, const String& body) {
    request.send(code, type, reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
}

// a String the caller is done with: the response takes it and sends it a piece at a time, no second copy of the whole
// body (the server's send() copies it: an ESP8266 with its modules had no room for both, /modules answered 500, W27)
void webSend(WebRequest& request, int code, const char* type, String&& body) {
    const auto length = static_cast<long>(body.length());
    request.send(code, type, length,
        [body = std::move(body)](uint8_t* out, size_t max, size_t index) -> size_t {
            const size_t size = body.length();
            const size_t take = (index < size) ? std::min(max, size - index) : 0;
            if (take) {
                std::memcpy(out, body.c_str() + index, take);
            }
            return take;
        });
}

void webSend(WebRequest& request, int code) {
    request.send(code, "text/plain", "");
}

uint16_t webPort() {
    return espurna::web::port;
}

WebServer& webServer() {
    return espurna::web::server;
}

void webBodyRegister(web_body_callback_f callback) {
    espurna::web::body_callbacks.push_back(callback);
}

void webRequestRegister(web_request_callback_f callback) {
    espurna::web::request_callbacks.push_back(callback);
}

void webLog(WebRequest& request) {
    DEBUG_MSG_P(PSTR("[WEBSERVER] %s - %s %s\n"),
        ipToString(IPAddress(request.remoteIp())).c_str(),
        request.methodName(),
        request.url());
}

void webSetup() {
    using namespace espurna::web;

    port = settings::port();
    log_requests = settings::access_log();

    auto& srv = server;
    srv.rewrite("/", "/index.html");

#if MODULES_SUPPORT
    // the webui module, or the head's own page (modules_page.h)
    srv.on("/index.html", WebMethod::Get, modulesOnHome);
#endif

    srv.on("/reset", WebMethod::Get, onReset);
    srv.on("/config", WebMethod::Get, onGetConfig);
    srv.on("/config", WebMethod::Post | WebMethod::Put, onPostConfig, onPostConfigBody);
    srv.on("/discover", WebMethod::Get, onDiscover);

    // the connectivity check redirects of the captive portal are the captive module's (captive.cpp). Every other
    // request (with its body: alexa) goes to the subscribers, else 404
    srv.onNotFound(onRequest, onBody);

#if DEBUG_SUPPORT
    if (log_requests) {
        srv.onAccess(webLog);
    }
#endif

    if (!srv.begin(port)) {
        DEBUG_MSG_P(PSTR("[WEBSERVER] Cannot listen on port %hu\n"), port);
        return;
    }

    DEBUG_MSG_P(PSTR("[WEBSERVER] Webserver running on port %hu\n"), port);
    espurnaRegisterLoop(espurna::web::loop);

    // new connections wait while the heap is low
    webGuardSetup();

    setupAccessControlHeaders();

    wsRegister()
        .onVisible(onVisible)
        .onKeyCheck(onKeyCheck);
}

#endif // WEB_SUPPORT
