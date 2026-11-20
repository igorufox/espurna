/*

Part of the API MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if WEB_SUPPORT
#include "api.h"
#include "web.h"
#include "ws.h"
#endif

// -----------------------------------------------------------------------------

namespace espurna {
namespace api {
namespace {

namespace param {

STRING_VIEW_INLINE(Key, "apikey");

} // namespace param

bool reserved_param(StringView value) {
    return value == param::Key;
}

namespace build {

constexpr bool enabled() {
    return 1 == API_ENABLED;
}

constexpr bool restful() {
    return 1 == API_RESTFUL;
}

STRING_VIEW_INLINE(Key, API_KEY);

constexpr StringView key() {
    return Key;
}

} // namespace build

namespace settings {
namespace keys {

STRING_VIEW_INLINE(Enabled, "apiEnabled");
STRING_VIEW_INLINE(Restful, "apiRestFul");
STRING_VIEW_INLINE(Key, "apiKey");

} // namespace keys

bool enabled() {
    return getSetting(keys::Enabled, build::enabled());
}

bool restful() {
    return getSetting(keys::Restful, build::restful());
}

espurna::string key() {
    return getSetting(keys::Key, build::key());
}

} // namespace settings

namespace web {
#if WEB_SUPPORT

STRING_VIEW_INLINE(Prefix, "api");

bool onKeyCheck(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(Prefix);
}

void onVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, Prefix);
}

void onConnected(espurna::json::Writer& root) {
    root.field(settings::keys::Enabled, apiEnabled());
    root.field(settings::keys::Key, apiKey());
    root.field(settings::keys::Restful, apiRestFul());
}

void setup() {
    wsRegister()
        .onVisible(onVisible)
        .onConnected(onConnected)
        .onKeyCheck(onKeyCheck);
}

bool authenticate_header(WebRequest& request, const espurna::string& key) {
    if (settings::enabled() && key.length()) {
        const char* header = request.header("Api-Key");
        if (header && key.equals(header)) {
            return true;
        }
    }

    return false;
}

bool authenticate_param(WebRequest& request, const espurna::string& key) {
    const auto name = espurna::api::param::Key.toString();
    const char* param = request.param(name.c_str());
    return param && key.equals(param);
}

bool authenticate(WebRequest& request) {
    const auto key = apiKey();
    if (key.length()) {
        return authenticate_header(request, key)
            || authenticate_param(request, key);
    }

    return false;
}

#endif
} // namespace web

} // namespace
} // namespace api
} // namespace espurna

#if WEB_SUPPORT
bool apiAuthenticateHeader(WebRequest& request, const espurna::string& key) {
    return espurna::api::web::authenticate_header(request, key);
}

bool apiAuthenticateParam(WebRequest& request, const espurna::string& key) {
    return espurna::api::web::authenticate_param(request, key);
}

bool apiAuthenticate(WebRequest& request) {
    return espurna::api::web::authenticate(request);
}
#endif

bool apiReservedParam(espurna::StringView value) {
    return espurna::api::reserved_param(value);
}

espurna::string apiKey() {
    return espurna::api::settings::key();
}

bool apiEnabled() {
    return espurna::api::settings::enabled();
}

bool apiRestFul() {
    return espurna::api::settings::restful();
}

void apiCommonSetup() {
#if WEB_SUPPORT
    espurna::api::web::setup();
#endif
}
