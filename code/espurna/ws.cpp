/*

WEBSOCKET: what the head keeps (docs/modules.md, modules_ws.h)

The WebSocket server of the WebUI is the ws module (ws_server.cpp, module_ws.cpp). Here: the callbacks the head's
parts and the modules register (wsRegister()) and the forwarders of the ws.h API (wsPost, wsSend, wsConnected ...) to
the module's table. No JSON here: the payload helpers of ws_utils.h are the tools module's (ws_utils.cpp). Without the module nobody is connected and nothing is sent.

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#if WEB_SUPPORT

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "system.h"
#include "utils.h"

#include "ws.h"
#include "modules_ws.h"

// -----------------------------------------------------------------------------
// Helpers / utility functions
// -----------------------------------------------------------------------------

namespace espurna {
namespace web {
namespace ws {

Callbacks& Callbacks::onVisible(Callbacks::OnSend cb, Callbacks::Prepend) {
    on_visible.insert(on_visible.begin(), cb);
    return *this;
}

Callbacks& Callbacks::onVisible(Callbacks::OnSend cb, Callbacks::Append) {
    on_visible.push_back(cb);
    return *this;
}

Callbacks& Callbacks::onConnected(Callbacks::OnSend cb, Callbacks::Prepend) {
    on_connected.insert(on_connected.begin(), cb);
    return *this;
}

Callbacks& Callbacks::onConnected(Callbacks::OnSend cb, Callbacks::Append) {
    on_connected.push_back(cb);
    return *this;
}

Callbacks& Callbacks::onData(Callbacks::OnSend cb, Callbacks::Prepend) {
    on_data.insert(on_data.begin(), cb);
    return *this;
}

Callbacks& Callbacks::onData(Callbacks::OnSend cb, Callbacks::Append) {
    on_data.push_back(cb);
    return *this;
}

Callbacks& Callbacks::onAction(Callbacks::OnAction cb, Callbacks::Prepend) {
    on_action.insert(on_action.begin(), cb);
    return *this;
}

Callbacks& Callbacks::onAction(Callbacks::OnAction cb, Callbacks::Append) {
    on_action.push_back(cb);
    return *this;
}

Callbacks& Callbacks::onKeyCheck(Callbacks::OnKeyCheck cb, Callbacks::Prepend) {
    on_keycheck.insert(on_keycheck.begin(), cb);
    return *this;
}

Callbacks& Callbacks::onKeyCheck(Callbacks::OnKeyCheck cb, Callbacks::Append) {
    on_keycheck.push_back(cb);
    return *this;
}

} // namespace ws
} // namespace web
} // namespace espurna

// -----------------------------------------------------------------------------
// The callbacks: kept here, the ws module sends them
// -----------------------------------------------------------------------------

namespace {

espurna::web::ws::Callbacks _ws_callbacks;

// the ws module's table, once it started
const WsModuleApi* _ws_api { nullptr };

} // namespace

espurna::web::ws::Callbacks& wsRegister() {
    return _ws_callbacks;
}

namespace {

std::vector<espurna::StringView> _ws_visible_modules;

} // namespace

void wsPayloadModule(espurna::json::Writer&, espurna::StringView name) {
    _ws_visible_modules.push_back(name);
}

std::vector<espurna::StringView> wsPayloadModulesTake() {
    std::vector<espurna::StringView> out;
    out.swap(_ws_visible_modules);
    return out;
}

void modulesWsProvide(const WsModuleApi* api) {
    _ws_api = api;
}

// -----------------------------------------------------------------------------
// The forwarders of ws.h
// -----------------------------------------------------------------------------

void wsPost(uint32_t client_id, espurna::web::ws::OnSend&& cb, size_t buffer_hint) {
    if (_ws_api) {
        _ws_api->postHint(client_id, std::move(cb), buffer_hint);
    }
}

void wsPost(uint32_t client_id, espurna::web::ws::OnSend&& cb) {
    if (_ws_api) {
        _ws_api->post(client_id, std::move(cb));
    }
}

void wsPost(espurna::web::ws::OnSend&& cb) {
    wsPost(0, std::move(cb));
}

void wsPost(uint32_t client_id, const espurna::web::ws::OnSend& cb, size_t buffer_hint) {
    wsPost(client_id, espurna::web::ws::OnSend(cb), buffer_hint);
}

void wsPost(uint32_t client_id, const espurna::web::ws::OnSend& cb) {
    wsPost(client_id, espurna::web::ws::OnSend(cb));
}

void wsPost(const espurna::web::ws::OnSend& cb) {
    wsPost(0, espurna::web::ws::OnSend(cb));
}

void wsPostManual(uint32_t client_id, espurna::web::ws::OnSend&& cb, size_t buffer_hint) {
    if (_ws_api) {
        _ws_api->postManualHint(client_id, std::move(cb), buffer_hint);
    }
}

void wsPostManual(uint32_t client_id, espurna::web::ws::OnSend&& cb) {
    if (_ws_api) {
        _ws_api->postManual(client_id, std::move(cb));
    }
}

void wsPostManual(espurna::web::ws::OnSend&& cb) {
    wsPostManual(0, std::move(cb));
}

void wsPostManual(uint32_t client_id, const espurna::web::ws::OnSend& cb, size_t buffer_hint) {
    wsPostManual(client_id, espurna::web::ws::OnSend(cb), buffer_hint);
}

void wsPostManual(uint32_t client_id, const espurna::web::ws::OnSend& cb) {
    wsPostManual(client_id, espurna::web::ws::OnSend(cb));
}

void wsPostManual(const espurna::web::ws::OnSend& cb) {
    wsPostManual(0, espurna::web::ws::OnSend(cb));
}

void wsPostAll(uint32_t client_id, espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    if (_ws_api) {
        _ws_api->postAll(client_id, std::move(cbs));
    }
}

void wsPostAll(espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    wsPostAll(0, std::move(cbs));
}

void wsPostAll(uint32_t client_id, const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    if (_ws_api) {
        _ws_api->postAllRef(client_id, cbs);
    }
}

void wsPostAll(const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    wsPostAll(0, cbs);
}

void wsPostSequence(uint32_t client_id, espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    if (_ws_api) {
        _ws_api->postSequence(client_id, std::move(cbs));
    }
}

void wsPostSequence(espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    wsPostSequence(0, std::move(cbs));
}

void wsPostSequence(uint32_t client_id, const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    if (_ws_api) {
        _ws_api->postSequenceRef(client_id, cbs);
    }
}

void wsPostSequence(const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    wsPostSequence(0, cbs);
}

void wsSend(espurna::web::ws::OnSend callback) {
    if (_ws_api) {
        _ws_api->sendAll(std::move(callback));
    }
}

void wsSend(uint32_t client_id, espurna::web::ws::OnSend callback) {
    if (_ws_api) {
        _ws_api->sendTo(client_id, std::move(callback));
    }
}

void wsSend(const char* data) {
    if (_ws_api) {
        _ws_api->sendText(data);
    }
}

WsClientInfo wsClientInfo(uint32_t client_id) {
    if (_ws_api) {
        return _ws_api->clientInfo(client_id);
    }

    return WsClientInfo{
        .connected = false,
        .stalled = false,
    };
}

void wsPause(bool paused) {
    if (_ws_api) {
        _ws_api->pause(paused);
    }
}

bool wsConnected() {
    return _ws_api && _ws_api->connected();
}

bool wsConnected(uint32_t client_id) {
    return _ws_api && _ws_api->connectedTo(client_id);
}

#if DEBUG_WEB_SUPPORT
bool wsDebugSend(const DebugPrefix& prefix, const char* message, size_t length) {
    return _ws_api && _ws_api->debugSend && _ws_api->debugSend(prefix, message, length);
}
#endif

#endif // WEB_SUPPORT
