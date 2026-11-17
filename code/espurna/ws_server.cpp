/*

THE WS MODULE (docs/modules.md): the WebSocket of the WebUI, every chip alike. The WebSocket server
(libs/websocket.h) on /ws of the head's web server, the /auth ticket, the queue of the posted messages, the periodic
status, the debug log, the settings and actions the WebUI sends. The WebUI module needs it.

The head keeps what its parts and the other modules call (ws.cpp): the callbacks they register (wsRegister()), the
payload helpers (ws_utils.h) and the forwarders of wsPost / wsSend / wsConnected ... to the table this module hands
over when it starts (modules_ws.h). Without the module nothing is sent and nobody is connected.

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#if WEB_SUPPORT && MODULE_BUILD

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <queue>
#include <vector>

#include "datetime.h"
#include "ntp.h"
#include "system.h"
#include "espurna_tasks.h"
#include "utils.h"
#include "web.h"
#include "espurna_wifi.h"

#include "ws.h"
#include "ws_internal.h"
#include "modules_ws.h"

#include "libs/websocket.h"
// the implementation lives in this module only (not in libs/net_impl.h of the head)
#include "libs/websocket_impl.h"

#include "espurna_chip.h"
#include "adc_driver.h"

namespace espurna {
namespace web {
namespace ws {

constexpr size_t PostponedCallback::DefaultBufferHint;

void PostponedCallback::Storage::Destructor::operator()(Callback& callback) const noexcept {
    callback.~Callback();
}

void PostponedCallback::Storage::Destructor::operator()(Storage::Pointer&) const noexcept {
}

void PostponedCallback::Storage::Destructor::operator()(Storage::Instance& obj) const noexcept {
    obj.obj.~Container();
}

PostponedCallback::Storage::Impl::Impl() :
    empty{}
{}

PostponedCallback::Storage::Impl::~Impl() {
}

PostponedCallback::Storage::Move::Move(Storage& storage) :
    _storage(storage)
{}

void PostponedCallback::Storage::Move::operator()(Callback& callback) const noexcept {
    ::new (&_storage._impl.callback) Callback(std::move(callback));
}

void PostponedCallback::Storage::Move::operator()(Storage::Pointer& ptr) const noexcept {
    ::new (&_storage._impl.pointer) Storage::Pointer(std::move(ptr));
    ptr.ptr = nullptr;
    ptr.offset = 0;
}

void PostponedCallback::Storage::Move::operator()(Storage::Instance& obj) const noexcept {
    ::new (&_storage._impl.instance) Storage::Instance(std::move(obj));
    obj.offset = 0;
}

PostponedCallback::Storage::~Storage() {
    visit(Destructor());
}

PostponedCallback::Storage::Storage(Storage&& other) noexcept :
    _type(other._type)
{
    other.visit(Move(*this));
}

namespace {

namespace build {

constexpr bool authentication() {
    return 1 == WS_AUTHENTICATION;
}

} // namespace build

namespace settings {
namespace keys {

STRING_VIEW_INLINE(Prefix, "ws");
STRING_VIEW_INLINE(Auth, "wsAuth");

} // namespace keys

bool authentication() {
    return getSetting(keys::Auth, build::authentication());
}

} // namespace settings

} // namespace

} // namespace ws
} // namespace web
} // namespace espurna

// -----------------------------------------------------------------------------
// Periodic updates
// -----------------------------------------------------------------------------

namespace {

void _wsUpdateAp(JsonObject& root) {
    IPAddress ip{};

    if (wifiConnectable()) {
        ip = wifiApIp();
    }

    root[F("apip")] = ipToString(ip);
}

void _wsUpdateSta(JsonObject& root) {
    IPAddress ip{};
    espurna::wifi::StaNetwork network{};

    if (wifiConnected()) {
        ip = wifiStaIp();
        network = wifiStaInfo();
    }

    root[F("ssid")] = network.ssid;
    root[F("bssid")] =
        ::espurna::settings::internal::serialize(network.bssid);
    root[F("channel")] = network.channel;
    root[F("staip")] = ipToString(ip);
}

void _wsUpdateStats(JsonObject& root) {
    const auto stats = systemHeapStats();
    root[F("heap")] = stats.available;
    root[F("heapUsable")] = stats.usable;
    root[F("heapFrag")] = stats.fragmentation;
    root[F("uptime")] = prettyDuration(systemUptime());
    root[F("rssi")] = WiFi.RSSI();
    root[F("loadaverage")] = systemLoadAverage();
#if ADC_MODE_VALUE == ADC_VCC
    root[F("vcc")] = espurna::adc::chip::vcc();
#endif
}

#if NTP_SUPPORT
void _wsUpdateTime(JsonObject& root) {
    if (!ntpSynced()) {
        return;
    }

    using namespace espurna::datetime;

    const auto ctx = make_context(time(nullptr));
    root[F("now")] = format_local_tz(ctx);
}
#endif

void _wsUpdate(JsonObject& root) {
    _wsUpdateAp(root);
    _wsUpdateSta(root);
    _wsUpdateStats(root);
#if NTP_SUPPORT
    _wsUpdateTime(root);
#endif
}

constexpr espurna::duration::Seconds WsUpdateInterval { WS_UPDATE_INTERVAL };
espurna::time::CoreClock::time_point _ws_last_update;

// wsPause(): the clients stay connected, only the periodic status and the debug log stop going out (every message is
// a heap buffer per client). Matter commissioning on Beken needs that heap for its certificate checks: a 4-relay bridge
// ran out of memory in wsSend() of the status update while the hub paired it
bool _ws_held { false };

void _wsResetUpdateTimer() {
    _ws_last_update = espurna::time::millis() + WsUpdateInterval;
}

void _wsDoUpdate(const bool connected) {
    if (!connected || _ws_held) {
        return;
    }

    auto ts = decltype(_ws_last_update)::clock::now();
    if (ts - _ws_last_update > WsUpdateInterval) {
        _ws_last_update = ts;
        wsSend(_wsUpdate);
    }
}

} // namespace

// -----------------------------------------------------------------------------
// WS callbacks
// -----------------------------------------------------------------------------

namespace {

bool _ws_auth { espurna::web::ws::build::authentication() };

// the one WebSocket server of ESPurna (libs/websocket.h), /ws of the web server
espurna::websocket::Server _ws;

std::queue<espurna::web::ws::PostponedCallback> _ws_queue;

// Every post waits on the heap until the client drains it. With a slow client and something changing fast
// (e.g. a flapping input toggling a relay several times a second) the queue would grow until allocation
// fails inside std::queue, which has no way to report it and crashes. Losing an update is the lesser evil
constexpr size_t QueueMax { 32 };
bool _ws_queue_dropping { false };

bool _wsQueueFull() {
    if (_ws_queue.size() < QueueMax) {
        if (_ws_queue.size() < (QueueMax / 2)) {
            _ws_queue_dropping = false;
        }
        return false;
    }

    // set before logging, debug output may be posted here as well
    if (!_ws_queue_dropping) {
        _ws_queue_dropping = true;
        DEBUG_MSG_P(PSTR("[WEBSOCKET] Queue is full, dropping updates\n"));
    }

    return true;
}

template <typename T>
void _wsPostCallbacks(uint32_t client_id, T&& cbs) {
    if (!_wsQueueFull()) {
        _ws_queue.emplace(client_id, espurna::web::ws::PostponedCallback::Storage(std::forward<T>(cbs)));
    }
}

template <typename T>
void _wsPostCallbacks(uint32_t client_id, T&& cbs, espurna::web::ws::PostponedCallback::Mode mode) {
    if (!_wsQueueFull()) {
        _ws_queue.emplace(client_id, espurna::web::ws::PostponedCallback::Storage(std::forward<T>(cbs)), mode);
    }
}

void _wsBufferHint(size_t buffer_hint) {
    _ws_queue.back().buffer_hint(buffer_hint);
}

} // namespace

void wsPost(uint32_t client_id, espurna::web::ws::OnSend&& cb, size_t buffer_hint) {
    const auto size = _ws_queue.size();
    wsPost(client_id, std::move(cb));
    if (_ws_queue.size() != size) {
        _wsBufferHint(buffer_hint);
    }
}

void wsPost(uint32_t client_id, espurna::web::ws::OnSend&& cb) {
    _wsPostCallbacks(client_id, std::move(cb));
}

void wsPost(espurna::web::ws::OnSend&& cb) {
    _wsPostCallbacks(0, std::move(cb));
}

void wsPost(uint32_t client_id, const espurna::web::ws::OnSend& cb, size_t buffer_hint) {
    const auto size = _ws_queue.size();
    wsPost(client_id, cb);
    if (_ws_queue.size() != size) {
        _wsBufferHint(buffer_hint);
    }
}

void wsPost(uint32_t client_id, const espurna::web::ws::OnSend& cb) {
    _wsPostCallbacks(client_id, cb);
}

void wsPost(const espurna::web::ws::OnSend& cb) {
    _wsPostCallbacks(0, cb);
}

void wsPostManual(uint32_t client_id, espurna::web::ws::OnSend&& cb) {
    _wsPostCallbacks(client_id, std::move(cb), espurna::web::ws::PostponedCallback::Mode::Manual);
}

void wsPostManual(espurna::web::ws::OnSend&& cb) {
    wsPostManual(0, std::move(cb));
}

void wsPostManual(uint32_t client_id, espurna::web::ws::OnSend&& cb, size_t buffer_hint) {
    const auto size = _ws_queue.size();
    wsPostManual(client_id, std::move(cb));
    if (_ws_queue.size() != size) {
        _wsBufferHint(buffer_hint);
    }
}

void wsPostManual(uint32_t client_id, const espurna::web::ws::OnSend& cb) {
    _wsPostCallbacks(client_id, cb, espurna::web::ws::PostponedCallback::Mode::Manual);
}

void wsPostManual(const espurna::web::ws::OnSend& cb) {
    wsPostManual(0, cb);
}

void wsPostManual(uint32_t client_id, const espurna::web::ws::OnSend& cb, size_t buffer_hint) {
    const auto size = _ws_queue.size();
    wsPostManual(client_id, cb);
    if (_ws_queue.size() != size) {
        _wsBufferHint(buffer_hint);
    }
}

void wsPostAll(uint32_t client_id, espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    _wsPostCallbacks(client_id, std::move(cbs), espurna::web::ws::PostponedCallback::Mode::All);
}

void wsPostAll(espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    wsPostAll(0, std::move(cbs));
}

void wsPostAll(uint32_t client_id, const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    _wsPostCallbacks(client_id, cbs, espurna::web::ws::PostponedCallback::Mode::All);
}

void wsPostAll(const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    wsPostAll(0, cbs);
}

void wsPostSequence(uint32_t client_id, espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    _wsPostCallbacks(client_id, std::move(cbs), espurna::web::ws::PostponedCallback::Mode::Sequence);
}

void wsPostSequence(espurna::web::ws::Callbacks::OnSendContainer&& cbs) {
    wsPostSequence(0, std::move(cbs));
}

void wsPostSequence(uint32_t client_id, const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    _wsPostCallbacks(client_id, cbs, espurna::web::ws::PostponedCallback::Mode::Sequence);
}

void wsPostSequence(const espurna::web::ws::Callbacks::OnSendContainer& cbs) {
    wsPostSequence(0, cbs);
}

// -----------------------------------------------------------------------------
// WS authentication
// -----------------------------------------------------------------------------

namespace {

constexpr size_t WsMaxClients { WS_MAX_CLIENTS };
constexpr espurna::duration::Seconds WsTimeout { WS_TIMEOUT };

WsTicket _ws_tickets[WsMaxClients];

// a ticket for the address of a logged in browser: its WebSocket is taken then
void _onAuth(WebRequest& request) {
    if (!webApModeRequest(request) && !webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    const IPAddress ip(request.remoteIp());
    auto now = WsTicket::TimeSource::now();

    auto it = std::begin(_ws_tickets);
    while (it != std::end(_ws_tickets)) {
        if (!((*it).ip != IPAddress())
            || ((*it).ip == ip)
            || (now - (*it).timestamp > WsTimeout)) {
            break;
        }
        ++it;
    }

    if (it != std::end(_ws_tickets)) {
        (*it).ip = ip;
        (*it).timestamp = now;
        request.send(200, "text/plain", "OK");
        return;
    }

    request.send(429, "text/plain", "Too many sessions");
}

void _wsAuthUpdate(const IPAddress& ip) {
    for (auto& ticket : _ws_tickets) {
        if (ticket.ip == ip) {
            ticket.timestamp = WsTicket::TimeSource::now();
            break;
        }
    }
}

bool _wsAuth(const IPAddress& ip) {
    auto now = WsTicket::TimeSource::now();

    for (auto& ticket : _ws_tickets) {
        if (ticket.ip == ip) {
            if (now - ticket.timestamp < WsTimeout) {
                return true;
            }
            return false;
        }
    }

    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// Debug
// -----------------------------------------------------------------------------

#if DEBUG_WEB_SUPPORT

namespace {

espurna::web::ws::PostponedDebug _ws_debug;

} // namespace

bool wsDebugSend(const DebugPrefix& prefix, const char* message, size_t length) {
    if (!_ws_held && (wifiConnected() || wifiApStations()) && wsConnected()) {
        _ws_debug.buffer(prefix, message, length);
        return true;
    }

    return false;
}

#endif

// -----------------------------------------------------------------------------
// Store indexed key (key0, key1, etc.) from array
// -----------------------------------------------------------------------------

namespace {

// Check the existing setting before saving it
// (we only care about the settings storage, don't mind the build values)
bool _wsStore(espurna::settings::kvs_type& instance, String key, const String& value) {
    const auto current = instance.get(key);
    if (!current || (current.ref() != value)) {
        return instance.set(key, value);
    }

    return false;
}

// TODO: generate "accepted" keys in the initial phase of the connection?
// TODO: is value ever used... by anything?
bool _wsCheckKey(const String& key, const JsonVariant& value) {
    for (auto& callback : wsRegister().on_keycheck) {
        if (callback(key, value)) {
            return true;
        }
    }

    return false;
}

void _wsPostParse(uint32_t client_id, bool save, bool reload) {
    if (save) {
        saveSettings();
        espurnaReload();

        wsPost(client_id, [save, reload](JsonObject& root) {
            if (reload) {
                root[F("action")] = F("reload");
            } else if (save) {
                root[F("saved")] = true;
            }
            root[F("message")] = F("Changes saved");
        });

        _wsResetUpdateTimer();
        return;
    }

    wsPost(client_id, [](JsonObject& root) {
        root[F("message")] = F("No changes detected");
    });
}

// by the client id and address: a deferred message is parsed in the loop, the client may be gone by then
void _wsParseImpl(uint32_t client_id, const IPAddress& ip, uint8_t* payload, size_t length) {
    //DEBUG_MSG_P(PSTR("[WEBSOCKET] Parsing: %.*s\n"),
    //    length, reinterpret_cast<cont char*>(payload));

    // Check early for empty object / nothing
    if ((length < 2) || (payload[0] != '{')) {
        return;
    }

    if ((length == 2) && (payload[1] == '}')) {
        return;
    }

    // Notice that the buffer payload above *is null terminated*!
    // JSON parser should be pretty efficient with the non-const payload,
    // most of the space is taken by the object key references
    auto* ptr = reinterpret_cast<char*>(payload);

    DynamicJsonBuffer jsonBuffer(512);
    JsonObject& root = jsonBuffer.parseObject(ptr);
    if (!root.success()) {
        wsPost(client_id, [](JsonObject& root) {
            root[F("message")] = F("JSON parsing error");
        });
        return;
    }

    // Check actions -----------------------------------------------------------

    const char* action = root["action"];
    if (action) {
        if (strcmp(action, "ping") == 0) {
            wsPost(client_id, [](JsonObject& root) {
                root["pong"] = 1;
            });
            _wsAuthUpdate(ip);
            return;
        }

        if (strcmp(action, "reboot") == 0) {
            prepareReset(CustomResetReason::Web);
            return;
        }

        if (strcmp(action, "reconnect") == 0) {
            wifiDisconnect();
            return;
        }

        if (strcmp(action, "factory_reset") == 0) {
            factoryReset();
            return;
        }

        JsonObject& data = root["data"];
        if (data.success()) {
            if (strcmp(action, "restore") == 0) {
                const auto message = settingsRestoreJson(data)
                    ? STRING_VIEW("Changes saved, you should be able to reboot now")
                    : STRING_VIEW("Cound not restore the configuration, see the debug log for more information");

                wsPost(client_id, [message](JsonObject& root) {
                    root[F("message")] = message;
                });
                return;
            }

            for (auto& callback : wsRegister().on_action) {
                callback(client_id, action, data);
            }
        }
    };

    // Update settings in-place. Unlike 'restore', this only
    // removes keys explicitly set in the 'del' list
    JsonObject& settings = root[F("settings")];
    if (!settings.success()) {
        return;
    }

    bool save { false };
    bool reload { false };

    JsonArray& toDelete = settings["del"];
    for (const auto& value : toDelete) {
        delSetting(value.as<String>());
    }

    auto& instance = espurna::settings::kvs_instance();

    // TODO: pass key as string, we always attempt to use it as such
    JsonObject& toAssign = settings["set"];
    for (auto& kv : toAssign) {
        const String key = kv.key;
        if (_wsCheckKey(key, kv.value)) {
            if (_wsStore(instance, key, kv.value.as<String>())) {
                save = true;
            }
        }
    }

    _wsPostParse(client_id, save, reload);
}

} // namespace

namespace {

// Any connected client with queued messages
bool _wsOutputPending() {
    return _ws.pending();
}

bool _wsOnKeyCheck(espurna::StringView key, const JsonVariant&) {
    return key.startsWith(espurna::web::ws::settings::keys::Prefix);
}

void _wsOnConnected(JsonObject& root) {
    root["webMode"] = WEB_MODE_NORMAL;

    const auto info = buildInfo();
    root[F("sdk")] = info.sdk.base.c_str();
    root[F("core")] = info.sdk.version.c_str();

    // device template search picks the database and the pin layout by 'arch' and 'mcu'
    espurna::chip::ws_connected(root);

    // build values, or the ones written by the device template
    root[F("manufacturer")] = systemManufacturer();
    root[F("device")] = systemDeviceName();

    root[F("app_name")] = info.app.name;
    root[F("app_version")] = info.app.version;
    root[F("app_build")] = info.app.build_time;
    // release image this build belongs to, WebUI OTA lists its files from GitHub ("" - local build)
    root[F("fwImage")] = espurna::chip::image();

    root[F("hostname")] = systemHostname();
    root[F("chipid")] = systemChipId().c_str();
    root[F("desc")] = systemDescription();

    root[F("sketch_size")] = ESP.getSketchSize();
    root[F("free_size")] = ESP.getFreeSketchSpace();
}

void _wsOnVisible(JsonObject& root) {
    root[espurna::web::ws::settings::keys::Auth] = _ws_auth ? 1 : 0;
#if ADC_MODE_VALUE == ADC_VCC
    // supply voltage is only known with the ADC measuring it, the row is hidden otherwise
    wsPayloadModule(root, STRING_VIEW("vcc"));
#endif
    espurna::chip::ws_visible(root);
}

void _wsConnected(uint32_t client_id) {
    if (systemPasswordMustChange()) {
        wsPost(client_id, [](JsonObject& root) {
            root["webMode"] = WEB_MODE_PASSWORD;
        }, JSON_OBJECT_SIZE(1));
        return;
    }

    wsPostAll(client_id, wsRegister().on_visible);
    wsPostSequence(client_id, wsRegister().on_connected);
    wsPostSequence(client_id, wsRegister().on_data);
}

// in the loop: the WebSocket server runs there (libs/websocket.h)
void _wsEvent(espurna::websocket::Event event, uint32_t id, uint32_t ip, const uint8_t* data, size_t len) {
    using espurna::websocket::Event;

    const IPAddress address(ip);
    switch (event) {
    case Event::Connect:
#ifndef NOWSAUTH
        if (_ws_auth && !_wsAuth(address)) {
            DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u session expired for %s\n"), id, ipToString(address).c_str());
            _ws.close(id);
            return;
        }
#endif
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u connected, ip: %s\n"), id, ipToString(address).c_str());
        _wsConnected(id);
        _wsResetUpdateTimer();
        break;

    case Event::Disconnect:
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u disconnected\n"), id);
        wifiApCheck();
        break;

    case Event::Message:
        // `data` ends with 0 after `len` (libs/websocket.h); the parser takes it in place
        _wsParseImpl(id, address, const_cast<uint8_t*>(data), len);
        break;
    }
}

// Contiguous heap kept free next to a WebSocket message (the message block, lwIP), on every chip. A failed
// allocation aborts the board: ESP32 with Matter (free heap 24 KB, lowest 632 B) restarted in
// AsyncWebSocket::makeBuffer 15 s after the start (11-11); the ESP8266 runs as low on heap with every module on
constexpr size_t WsSendHeadroom { 1536 };
constexpr size_t WsMessageHeadroom { 512 };

// a message to one client printed and queued; false: no heap for it now (nothing queued, the caller may try again)
bool _wsSendTo(uint32_t client_id, JsonObject& root);

// after a message of the queue did not go out: the next try not before this (not every loop pass)
constexpr espurna::duration::Milliseconds WsRetryInterval { 100 };
espurna::time::CoreClock::time_point _ws_queue_failed;
bool _ws_queue_waiting { false };

// the next one counts its timeout from the moment it is first in line, not from when it was queued: the initial
// messages of a second WebUI tab wait behind the first one's and were dropped unsent (10 s from the connect, W70)
void _wsQueuePop() {
    _ws_queue.pop();
    if (!_ws_queue.empty()) {
        _ws_queue.front().touch();
    }
}

void _wsHandlePostponedCallbacks(bool connected) {
    // TODO: make this generic loop method to queue important ws messages?
    //       or, if something uses ticker / async ctx to send messages,
    //       it needs a retry mechanism built into the callback object
    if (!connected && !_ws_queue.empty()) {
        _wsQueuePop();
        return;
    }

    if (_ws_queue.empty()) {
        return;
    }

    auto& callbacks = _ws_queue.front();

    // avoid stalling forever when can't send anything: 10 s without a message going out
    using TimeSource = espurna::time::CpuClock;
    using CpuSeconds = std::chrono::duration<TimeSource::rep>;

    constexpr CpuSeconds WsQueueTimeoutClockCycles { 10 };
    if (TimeSource::now() - callbacks.start() > WsQueueTimeoutClockCycles) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u queued message timed out\n"), callbacks.id());
        _wsQueuePop();
        return;
    }

    // client id equal to 0 means we need to send the message to every client
    if (callbacks.id()) {
        // ...but, we need to check if client is still connected
        if (!_ws.hasClient(callbacks.id())) {
            _wsQueuePop();
            return;
        }

        // wait until we can send the next batch of messages
        // XXX: enforce that callbacks send only one message per iteration
        if (!_ws.availableForWrite(callbacks.id())) {
            return;
        }
    }

    const auto now = espurna::time::CoreClock::now();
    if (_ws_queue_waiting && (now - _ws_queue_failed < WsRetryInterval)) {
        return;
    }
    _ws_queue_waiting = false;

    // the JSON buffer and then the message need that much contiguous heap: wait for it instead of aborting
    if (systemHeapStats().usable < (callbacks.buffer_hint() + WsSendHeadroom)) {
        return;
    }

    // XXX: block allocation will try to create *2 next time,
    // likely failing and causing wsSend to reference empty objects
    // XXX: arduinojson6 will not do this, but we may need to use per-callback buffers
    DynamicJsonBuffer jsonBuffer(callbacks.buffer_hint());
    JsonObject& root = jsonBuffer.createObject();

    callbacks.send(root);

    using Mode = decltype(callbacks.mode());

    switch (callbacks.mode()) {
    case Mode::Manual:
        break;

    case Mode::All:
    case Mode::Sequence:
        if (callbacks.id()) {
            // the initial messages of a WebUI page (its panels, the relays): one lost leaves the page without that
            // part for good (W70). Not out now - the same callback again, once the heap is back
            if (!root.success() || !_wsSendTo(callbacks.id(), root)) {
                callbacks.rewind();
                _ws_queue_failed = now;
                _ws_queue_waiting = true;
                return;
            }
            callbacks.touch();
        } else {
            wsSend(root);
        }
        yield();
        break;
    }

    if (callbacks.done()) {
        _wsQueuePop();
    }
}

void _wsLoop() {
    _ws.loop();
    const auto connected = wsConnected();
    _wsDoUpdate(connected);
    _wsHandlePostponedCallbacks(connected);
}

} // namespace

// -----------------------------------------------------------------------------
// ArduinoJson <-> WS: the message printed into one block, sent as one frame
// -----------------------------------------------------------------------------

namespace {

std::unique_ptr<char[]> _wsPrint(JsonObject& root, size_t len) {
    std::unique_ptr<char[]> out(new (std::nothrow) char[len + 1]);
    if (out) {
        root.printTo(out.get(), len + 1);
    }
    return out;
}

// a client that is gone needs no second try: true
bool _wsSendTo(uint32_t client_id, JsonObject& root) {
    if (!_ws.hasClient(client_id)) {
        return true;
    }

    // tried again every WsRetryInterval while the heap is short: no log line each time (the debug log goes to the
    // WebSocket as well and takes heap of its own)
    const auto len = root.measureLength();
    if (systemHeapStats().usable < (len + WsMessageHeadroom)) {
        return false;
    }

    auto buffer = _wsPrint(root, len);
    return buffer && _ws.text(client_id, buffer.get(), len);
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

WsClientInfo wsClientInfo(uint32_t client_id) {
    const auto connected = _ws.hasClient(client_id);

    return WsClientInfo{
        .connected = connected,
        .stalled = connected && !_ws.availableForWrite(client_id),
    };
}

// Clients stay connected (closing them showed "Connection lost" in the middle of a Matter pairing); the periodic status
// and the debug log are held back until the end, ref. _ws_held
void wsPause(bool paused) {
    if (_ws_held == paused) {
        return;
    }

    if (!paused) {
        _ws_held = false;
        _wsResetUpdateTimer();
    }
    DEBUG_MSG_P(PSTR("[WEBSOCKET] %s\n"), paused ? "Status and log updates held" : "Status and log updates resumed");
    _ws_held = paused;
}

bool wsConnected() {
    return (_ws.count() > 0);
}

bool wsConnected(uint32_t client_id) {
    return _ws.hasClient(client_id);
}

// Note: 'measurement' tries to serialize json contents byte-by-byte by using a dummy printer
// Make sure there is no off-by-one errors, since *some* output impelementations inject '\0'

void wsSend(JsonObject& root) {
    const auto len = root.measureLength();
    if (systemHeapStats().usable < (len + WsMessageHeadroom)) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] No heap for %u bytes, message dropped\n"), len);
        return;
    }
    auto buffer = _wsPrint(root, len);
    if (buffer) {
        _ws.textAll(buffer.get(), len);
    }
}

void wsSend(uint32_t client_id, JsonObject& root) {
    if (!_ws.hasClient(client_id)) {
        return;
    }

    const auto len = root.measureLength();
    if (systemHeapStats().usable < (len + WsMessageHeadroom)) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u no heap for %u bytes, message dropped\n"), client_id, len);
        return;
    }
    auto buffer = _wsPrint(root, len);
    if (buffer) {
        _ws.text(client_id, buffer.get(), len);
    }
}

void wsSend(espurna::web::ws::OnSend callback) {
    if (_ws.count() > 0) {
        DynamicJsonBuffer jsonBuffer(512);
        JsonObject& root = jsonBuffer.createObject();
        callback(root);

        wsSend(root);
    }
}

void wsSend(const char * payload) {
    if (_ws.count() > 0) {
        _ws.textAll(payload, strlen(payload));
    }
}

void wsSend(uint32_t client_id, espurna::web::ws::OnSend callback) {
    if (!_ws.hasClient(client_id)) return;

    DynamicJsonBuffer jsonBuffer(512);
    JsonObject& root = jsonBuffer.createObject();
    callback(root);
    wsSend(client_id, root);
}

void wsSend(uint32_t client_id, const char* payload) {
    _ws.text(client_id, payload, strlen(payload));
}

void wsSetup() {
    _ws.clientsMax(WsMaxClients);
    _ws.onEvent(_wsEvent);

    // /ws of the web server: the WebSocket server takes the connection
    webServer().onUpgrade([](WebRequest& request) {
        if (strcmp(request.url(), "/ws") != 0) {
            request.send(404, "text/plain", "Not Found");
            return;
        }
        _ws.accept(request);
    });

    _ws_auth = espurna::web::ws::settings::authentication();
    webServer().on("/auth", WebMethod::Get, _onAuth);

    // first in the lists: the head's parts registered theirs at their setup, before this module started
    using Prepend = espurna::web::ws::Callbacks::Prepend;
    wsRegister()
        .onConnected(_wsOnConnected, Prepend{})
        .onVisible(_wsOnVisible, Prepend{})
        .onKeyCheck(_wsOnKeyCheck);

    // e.g. RESET command reply, don't restart before it reaches the client
    espurna::system::registerPendingOutput([]() {
        return _wsOutputPending();
    });

    espurnaRegisterLoop(_wsLoop);
}

#endif // WEB_SUPPORT && MODULE_BUILD
