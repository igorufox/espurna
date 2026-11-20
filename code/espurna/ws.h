/*

WEBSOCKET: the API of the WebUI's WebSocket. The server is the ws module (ws_server.cpp); every call below goes
through the head's forwarders (ws.cpp, modules_ws.h), without the module nobody is connected and nothing is sent

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once

#include "json.h"

#include <functional>
#include <vector>

#include "web.h"
#include "utils.h"

#include "ws_utils.h"

// Generalized WS lifetime callbacks.
// Callback could either be a lambda or a plain function pointer
//
// Connection start:
// - on_visible will be the very first message sent, callback data will be grouped together
// - on_connected is sent next, but each callback's data will be sent separately
// - on_data is the final one, each callback is executed separately
//
// While connected:
// - on_action will be ran whenever we receive special JSON 'action' payload
// - on_keycheck will be used to determine if we can handle specific settings keys

namespace espurna {
namespace web {
namespace ws {

// the writer is inside the message object: a callback adds its members (json.h, the tools module)
using OnSend = std::function<void(json::Writer& root)>;
// `data`: the "data" member of the message (an object, or invalid when there is none)
using OnAction = std::function<void(uint32_t client_id, const char* action, json::Value data)>;
using OnKeyCheck = std::function<bool(espurna::StringView key, json::Value value)>;

struct Callbacks {
    struct Prepend {
    };

    struct Append {
    };

    using OnSend = void(*)(json::Writer&);

    Callbacks& onVisible(OnSend, Append);
    Callbacks& onVisible(OnSend, Prepend);
    Callbacks& onVisible(OnSend f) {
        return onVisible(f, Append{});
    }

    Callbacks& onConnected(OnSend, Append);
    Callbacks& onConnected(OnSend, Prepend);
    Callbacks& onConnected(OnSend f) {
        return onConnected(f, Append{});
    }

    Callbacks& onData(OnSend, Append);
    Callbacks& onData(OnSend, Prepend);
    Callbacks& onData(OnSend f) {
        return onData(f, Append{});
    }

    using OnAction = void(*)(uint32_t, const char*, json::Value);

    Callbacks& onAction(OnAction, Append);
    Callbacks& onAction(OnAction, Prepend);
    Callbacks& onAction(OnAction f) {
        return onAction(f, Append{});
    }

    using OnKeyCheck = bool(*)(espurna::StringView, json::Value);

    Callbacks& onKeyCheck(OnKeyCheck, Append);
    Callbacks& onKeyCheck(OnKeyCheck, Prepend);

    Callbacks& onKeyCheck(OnKeyCheck f) {
        return onKeyCheck(f, Append{});
    }

    using OnSendContainer = std::vector<OnSend>;
    using OnActionContainer = std::vector<OnAction>;
    using OnKeyCheckContainer = std::vector<OnKeyCheck>;

    OnSendContainer on_visible;
    OnSendContainer on_connected;
    OnSendContainer on_data;

    OnActionContainer on_action;
    OnKeyCheckContainer on_keycheck;
};

} // namespace ws
} // namespace web
} // namespace espurna

// Postponed debug messages. best-effort, will not be re-scheduled when ws queue is full

bool wsDebugSend(const DebugPrefix&, const char*, size_t);

// Postponed json messages. schedules callback(s) to be called when resources to do so are available.
// Queued item is removed on client disconnection *or* when internal timeout occurs

// There are two policies set on how to send the data:
// - All writes every callback into the same message
// - Sequence sends a message of its own for each callback
// - Manual works only with a single callback object
//   However, it is up to the user to call `wsSend()`
//
// `wsPost()` callback uses *All mode by default
// (however, it is not currently possible to amend the created list object)
//
// WARNING: callback lists are taken by reference! make sure that list is ether:
// - std::move(...)'ed to give control of the callback list to us
// - persistent and will be available after the current block ends (global, heap-allocated, etc.)
//   de-allocation is not expected e.g. referenced struct from `wsRegister()` is never destroyed

void wsPost(uint32_t client_id, espurna::web::ws::OnSend&& cb, size_t buffer_hint);
void wsPost(uint32_t client_id, espurna::web::ws::OnSend&& cb);
void wsPost(espurna::web::ws::OnSend&& cb);

void wsPost(uint32_t client_id, const espurna::web::ws::OnSend& cb, size_t buffer_hint);
void wsPost(uint32_t client_id, const espurna::web::ws::OnSend& cb);
void wsPost(const espurna::web::ws::OnSend& cb);

void wsPostManual(uint32_t client_id, espurna::web::ws::OnSend&& cb, size_t buffer_hint);
void wsPostManual(uint32_t client_id, espurna::web::ws::OnSend&& cb);
void wsPostManual(espurna::web::ws::OnSend&& cb);

void wsPostManual(uint32_t client_id, const espurna::web::ws::OnSend& cb, size_t buffer_hint);
void wsPostManual(uint32_t client_id, const espurna::web::ws::OnSend& cb);
void wsPostManual(const espurna::web::ws::OnSend& cb);

void wsPostAll(uint32_t client_id, espurna::web::ws::Callbacks::OnSendContainer&& cbs);
void wsPostAll(espurna::web::ws::Callbacks::OnSendContainer&& cbs);

void wsPostAll(uint32_t client_id, const espurna::web::ws::Callbacks::OnSendContainer& cbs);
void wsPostAll(const espurna::web::ws::Callbacks::OnSendContainer& cbs);

void wsPostSequence(uint32_t client_id, espurna::web::ws::Callbacks::OnSendContainer&& cbs);
void wsPostSequence(espurna::web::ws::Callbacks::OnSendContainer&& cbs);

void wsPostSequence(uint32_t client_id, const espurna::web::ws::Callbacks::OnSendContainer& cbs);
void wsPostSequence(const espurna::web::ws::Callbacks::OnSendContainer& cbs);

// Right now: the callback writes the message, it goes to every client (or `client_id`) at once.
// May silently fail when network is busy sending previous requests, or there's not enough RAM

void wsSend(espurna::web::ws::OnSend callback);
void wsSend(uint32_t client_id, espurna::web::ws::OnSend callback);
void wsSend(const char* data);

// Check if any or specific client_id is connected
// Server will try to set unique ID for each client

struct WsClientInfo {
    bool connected;
    bool stalled;
};

WsClientInfo wsClientInfo(uint32_t client_id);

bool wsConnected();
void wsPause(bool);
bool wsConnected(uint32_t client_id);

// Append module's name that webui can make it's widgets visible
// (for the payload in `on_send` callback(s)): kept until the message ends, then one "modulesVisible" list (tools)
void wsPayloadModule(espurna::json::Writer&, espurna::StringView);
// the names collected by wsPayloadModule() since the last call (the head keeps them, no JSON there)
std::vector<espurna::StringView> wsPayloadModulesTake();
// the ws module (tools), at the end of a message: "modulesVisible": [<the names taken>]
void wsPayloadModulesFlush(espurna::json::Writer&);

// Access to our module-specific lifetime callbacks.
// Expected usage is through the on() methods. Kept by the head, the ws module sends them (modules_ws.h)

espurna::web::ws::Callbacks& wsRegister();

// the ws module's start (ws_server.cpp, from module_ws.cpp)
void wsSetup();
// the WebUI parts of the head that read JSON, the ws module's (ws_head.cpp)
void wsHeadSetup();

