/*

The restore of a settings backup (the JSON file of GET /config): the ws module, the head reads no JSON.
POST /config (the WebUI's "Restore" button) and the WebSocket action "restore".

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#if MODULE_BUILD && WEB_SUPPORT

#include <algorithm>
#include <memory>
#include <vector>

#include "json.h"
#include "settings.h"
#include "system.h"
#include "web.h"

bool settingsRestoreJson(espurna::json::Value data) {
    // Note: we try to match what /config generates, expect {"app":"ESPURNA",...}
    const auto app = data["app"];
    if (!app.isString()) {
        DEBUG_MSG_P(PSTR("[SETTING] Missing 'app' key\n"));
        return false;
    }

    const auto build_app = buildApp().name;
    if (build_app != app.view()) {
        DEBUG_MSG_P(PSTR("[SETTING] Invalid 'app' key\n"));
        return false;
    }

    // .../config will add this key, but it is optional
    if (data["backup"].asBool()) {
        resetSettings();
    }

    // These three are just metadata, no need to actually store them
    data.members([](espurna::StringView key, espurna::json::Value value) {
        if (key.startsWith(STRING_VIEW("app"))
            || key.startsWith(STRING_VIEW("version"))
            || key.startsWith(STRING_VIEW("backup")))
        {
            return;
        }

        const auto text = value.asString();
        setSetting(espurna::string(key), espurna::string(text.c_str()));
    });

    saveSettings();

    DEBUG_MSG_P(PSTR("[SETTINGS] Settings restored successfully\n"));
    return true;
}

namespace {

constexpr size_t ConfigBufferMax { 4096 };

struct ConfigUpload {
    std::vector<uint8_t> data;
    bool failed { false };
};

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
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    auto* upload = static_cast<ConfigUpload*>(request.context());
    bool success = false;
    if (upload && !upload->failed && !upload->data.empty()) {
        const auto root = espurna::json::parse(espurna::StringView(
            reinterpret_cast<const char*>(upload->data.data()), upload->data.size()));
        if (root.isObject()) {
            success = settingsRestoreJson(root);
        } else {
            DEBUG_MSG_P(PSTR("[SETTINGS] JSON parsing error\n"));
        }
    }

    request.send(success ? 200 : 400, "text/plain", success ? "OK" : "Error");
}

} // namespace

void settingsJsonSetup() {
    webServer().on("/config", WebMethod::Post | WebMethod::Put, onPostConfig, onPostConfigBody);
}

#endif // MODULE_BUILD && WEB_SUPPORT
