/*

WEBSOCKET: what the head keeps (docs/modules.md, modules_ws.h)

The WebSocket server of the WebUI is the ws module (ws_server.cpp, module_ws.cpp). Here: the callbacks the head's
parts and the modules register (wsRegister()), the payload helpers of ws_utils.h, and the forwarders of the ws.h API
(wsPost, wsSend, wsConnected ...) to the module's table. Without the module nobody is connected and nothing is sent.

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

namespace {

namespace internal {

STRING_VIEW_INLINE(SchemaKey, "schema");

} // namespace internal

} // namespace

EnumerableConfig::EnumerableConfig(JsonObject& root, StringView name) :
    _root(root.createNestedObject(name))
{}

void EnumerableConfig::replacement(SourceFunc source, TargetFunc target) {
    _replacements.push_back(
        Replacement{
            .source = source,
            .target = target,
        });
}

void EnumerableConfig::operator()(StringView name, espurna::settings::Iota iota, Check check, Setting* begin, Setting* end) {
    JsonArray& entries = _root.createNestedArray(name);

    if (_root.containsKey(internal::SchemaKey)) {
        return;
    }

    JsonArray& schema = _root.createNestedArray(internal::SchemaKey);
    for (auto it = begin; it != end; ++it) {
        schema.add((*it).prefix());
    }

    while (iota) {
        if (!check || check(*iota)) {
            JsonArray& entry = entries.createNestedArray();
            for (auto it = begin; it != end; ++it) {
                auto func = (*it).func();

                auto replacement = std::find_if(
                    _replacements.begin(),
                    _replacements.end(),
                    [&](const Replacement& replacement) {
                        return func == replacement.source;
                    });

                if (replacement != _replacements.end()) {
                    (*replacement).target(entry, *iota);
                } else {
                    entry.add(func(*iota));
                }
            }
        }

        ++iota;
    }
}

EnumerablePayload::EnumerablePayload(JsonObject& root, StringView name) :
    _root(root.createNestedObject(name))
{}

void EnumerablePayload::operator()(StringView name, espurna::settings::Iota iota, Check check, Pairs&& pairs) {
    JsonArray& entries = _root.createNestedArray(name);

    if (_root.containsKey(internal::SchemaKey)) {
        return;
    }

    JsonArray& schema = _root.createNestedArray(internal::SchemaKey);

    const auto begin = std::begin(pairs);
    const auto end = std::end(pairs);
    for (auto it = begin; it != end; ++it) {
        schema.add((*it).name);
    }

    while (iota) {
        if (!check || check(*iota)) {
            JsonArray& entry = entries.createNestedArray();
            for (auto it = begin; it != end; ++it) {
                (*it).generate(entry, *iota);
            }
        }

        ++iota;
    }
}

EnumerableTypes::EnumerableTypes(JsonObject& root, StringView name) :
    _root(root.createNestedArray(name))
{}

void EnumerableTypes::operator()(int value, StringView text) {
    auto& entry = _root.createNestedArray();
    entry.add(value);
    entry.add(text);
}

PostponedPayload::PostponedPayload() = default;

PostponedPayload::Flag::~Flag() {
    if (_ref._pending) {
        _ref._data = String();
        _ref._count = 0;
    }

    _ref._pending = false;
}

PostponedPayload::Flag::Flag(PostponedPayload& ref) :
    _ref(ref)
{
    _ref._pending = true;
}

bool PostponedPayload::connected() const {
    return _id ? wsConnected(_id) : wsConnected();
}

std::shared_ptr<PostponedPayload::Flag> PostponedPayload::make_flag() {
    if (!_pending) {
        return std::make_shared<Flag>(*this);
    }


    return nullptr;
}

bool PostponedPayload::post(bool connected) {
    if (!connected) {
        if (_data.length()) {
            _data = String();
        }

        _count = 0;
        _pending = false;

        return false;
    }

    if (connected && !_pending && _count) {
        auto flag = make_flag();
        auto cb = [flag](JsonObject& root) {
            if (flag->pending()) {
                auto& log = root.createNestedArray("log");
                log.add(flag->data());
            }
        };

        wsPost(_id, std::move(cb), BufferHint);
        return true;
    }

    return false;
}

bool PostponedPayload::post() {
    return post(connected());
}

void PostponedPayload::buffer(const char* data, size_t size) {
    if (!connected()) {
        return;
    }

    if (_count < CountMax) {
        buffer_impl(data, size);
        ++_count;
    }

    post();
}

void PostponedPayload::buffer_impl(StringView data) {
    _data.concat(data.data(), data.length());
}

void PostponedPayload::buffer_impl(const char* data, size_t length) {
    _data.concat(data, length);
}

void PostponedDebug::buffer(const DebugPrefix& prefix, const char* message, size_t length) {
    if (!connected()) {
        return;
    }

    if (_count < CountMax) {
        const auto prefixLen = debugPrefixLength(prefix);
        const auto bufferLen = _data.length()
            + prefixLen + length;

        _data.reserve(bufferLen);

        if (prefixLen) {
            buffer_impl(prefix, prefixLen);
        }

        buffer_impl(message, length);
        ++_count;
    }

    post();
}

constexpr duration::Milliseconds InplacePayload::DefaultWait;
constexpr duration::Seconds InplacePayload::DefaultTimeout;

InplacePayload::InplacePayload(JsonObject& root, uint32_t id) :
    _root(root),
    _id(id)
{}

void InplacePayload::reset() {
    if (_data.length()) {
        _data = String();
    }

    _count = 0;
}

bool InplacePayload::connected() const {
    return wsConnected(_id);
}

void InplacePayload::write_impl(const char* data, size_t length) {
    if (!connected()) {
        return;
    }

    if (_count > CountMax) {
        return;
    }

    _data.concat(data, length);
    ++_count;
}

void InplacePayload::write(const char* data, size_t length) {
    write_impl(data, length);
    send();
}

bool InplacePayload::can_send() const {
    return connected() && _count && _data.length();
}

bool InplacePayload::poll_send() {
    if (!can_send()) {
        reset();
        return false;
    }

    auto start = Clock::now();

    while (Clock::now() - start < _timeout) {
        auto info = wsClientInfo(_id);
        if (!info.connected) {
            reset();
            return false;
        }

        if (!info.stalled) {
            return true;
        }

        time::blockingDelay(_wait);
    }

    return false;
}

bool InplacePayload::send() {
    if (poll_send()) {
        send_impl();
        return true;
    }

    return false;
}

void InplacePayload::send_impl() {
    wsSend(_id, _root);
    reset();
}

InplaceLog::InplaceLog(JsonObject& root, uint32_t id) :
    InplacePayload(root, id),
    _log(root.createNestedArray("log"))
{}

void InplaceLog::write(const char* data, size_t length) {
    write_impl(data, length);
    send();
}

bool InplaceLog::send() {
    if (poll_send()) {
        if (_log.size()) {
            _log[0] = _data.c_str();
        } else {
            _log.add(_data.c_str());
        }

        send_impl();
        return true;
    }

    return false;
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

// Append module's name that webui can make it's widgets visible
void wsPayloadModule(JsonObject& root, espurna::StringView name) {
    STRING_VIEW_INLINE(Key, "modulesVisible");
    JsonArray& modules = root.containsKey(Key)
        ? root[Key]
        : root.createNestedArray(Key);
    modules.add(name);
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

void wsSend(JsonObject& root) {
    if (_ws_api) {
        _ws_api->sendAll(root);
    }
}

void wsSend(uint32_t client_id, JsonObject& root) {
    if (_ws_api) {
        _ws_api->sendTo(client_id, root);
    }
}

void wsSend(espurna::web::ws::OnSend callback) {
    if (wsConnected()) {
        DynamicJsonBuffer jsonBuffer(512);
        JsonObject& root = jsonBuffer.createObject();
        callback(root);
        wsSend(root);
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
