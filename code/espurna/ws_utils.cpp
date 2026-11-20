/*

The payload helpers of the WebUI (ws_utils.h): the tools module, the head has no JSON

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if MODULE_BUILD && WEB_SUPPORT

#include <algorithm>
#include <vector>

#include "ws.h"
#include "ws_utils.h"

namespace espurna {
namespace web {
namespace ws {
namespace {

STRING_VIEW_INLINE(SchemaKey, "schema");

} // namespace

EnumerableConfig::EnumerableConfig(json::Writer& root, StringView name) :
    _root(root)
{
    _root.object(name);
}

EnumerableConfig::~EnumerableConfig() {
    _root.end();
}

void EnumerableConfig::replacement(SourceFunc source, TargetFunc target) {
    _replacements.push_back(
        Replacement{
            .source = source,
            .target = target,
        });
}

void EnumerableConfig::operator()(StringView name, espurna::settings::Iota iota, Check check, Setting* begin, Setting* end) {
    _root.array(name);
    while (iota) {
        if (!check || check(*iota)) {
            _root.beginArray();
            for (auto it = begin; it != end; ++it) {
                auto func = (*it).func();

                auto replacement = std::find_if(
                    _replacements.begin(),
                    _replacements.end(),
                    [&](const Replacement& replacement) {
                        return func == replacement.source;
                    });

                if (replacement != _replacements.end()) {
                    (*replacement).target(_root, *iota);
                } else {
                    const auto value = func(*iota);
                    _root.value(StringView(value));
                }
            }
            _root.end();
        }

        ++iota;
    }
    _root.end();

    if (_schema) {
        return;
    }

    _schema = true;
    _root.array(SchemaKey);
    for (auto it = begin; it != end; ++it) {
        _root.value((*it).prefix());
    }
    _root.end();
}

EnumerablePayload::EnumerablePayload(json::Writer& root, StringView name) :
    _root(root)
{
    _root.object(name);
}

EnumerablePayload::~EnumerablePayload() {
    _root.end();
}

void EnumerablePayload::operator()(StringView name, espurna::settings::Iota iota, Check check, Pairs&& pairs) {
    const auto begin = std::begin(pairs);
    const auto end = std::end(pairs);

    _root.array(name);
    while (iota) {
        if (!check || check(*iota)) {
            _root.beginArray();
            for (auto it = begin; it != end; ++it) {
                (*it).generate(_root, *iota);
            }
            _root.end();
        }

        ++iota;
    }
    _root.end();

    if (_schema) {
        return;
    }

    _schema = true;
    _root.array(SchemaKey);
    for (auto it = begin; it != end; ++it) {
        _root.value((*it).name);
    }
    _root.end();
}

EnumerableTypes::EnumerableTypes(json::Writer& root, StringView name) :
    _root(root)
{
    _root.array(name);
}

EnumerableTypes::~EnumerableTypes() {
    _root.end();
}

void EnumerableTypes::operator()(int value, StringView text) {
    _root.beginArray();
    _root.value(value);
    _root.value(text);
    _root.end();
}

PostponedPayload::PostponedPayload() = default;

PostponedPayload::Flag::~Flag() {
    if (_ref._pending) {
        _ref._data = espurna::string();
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
            _data = espurna::string();
        }

        _count = 0;
        _pending = false;

        return false;
    }

    if (connected && !_pending && _count) {
        auto flag = make_flag();
        auto cb = [flag](json::Writer& root) {
            if (flag->pending()) {
                root.array(STRING_VIEW("log"));
                root.value(flag->data());
                root.end();
            }
        };

        wsPost(_id, std::move(cb));
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

InplacePayload::InplacePayload(uint32_t id) :
    _id(id)
{}

void InplacePayload::reset() {
    if (_data.length()) {
        _data = espurna::string();
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
    const auto& data = _data;
    wsSend(_id, [&](json::Writer& root) {
        root.array(STRING_VIEW("log"));
        root.value(StringView(data));
        root.end();
    });
    reset();
}

} // namespace ws
} // namespace web
} // namespace espurna

void wsPayloadModulesFlush(espurna::json::Writer& root) {
    const auto names = wsPayloadModulesTake();
    if (names.empty()) {
        return;
    }

    root.array(STRING_VIEW("modulesVisible"));
    for (const auto& name : names) {
        root.value(name);
    }
    root.end();
}

#endif // MODULE_BUILD && WEB_SUPPORT
