/*

THINGSPEAK MODULE

Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#if THINGSPEAK_SUPPORT

#include "mqtt.h"
#include "relay.h"
#include "rpc.h"
#include "sensor.h"
#include "thingspeak.h"
#include "ws.h"

#include <memory>

#include "libs/http_client.h"

namespace espurna {
namespace thingspeak {
namespace {

using TimeSource = espurna::time::CoreClock;

} // namespace

namespace build {

static constexpr size_t Unset { 0 };

static constexpr size_t Fields { THINGSPEAK_FIELDS };

static constexpr auto FlushInterval = espurna::duration::Milliseconds(THINGSPEAK_MIN_INTERVAL);
static constexpr size_t Retries { THINGSPEAK_TRIES };
static constexpr size_t BufferSize { 256 };

PROGMEM_STRING(ApiKey, THINGSPEAK_APIKEY);
PROGMEM_STRING(Address, THINGSPEAK_ADDRESS);

constexpr bool enabled() {
    return 1 == THINGSPEAK_ENABLED;
}

constexpr bool clearCache() {
    return 1 == THINGSPEAK_CLEAR_CACHE;
}

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Enabled, "tspkEnabled");
PROGMEM_STRING(ApiKey, "tspkKey");
PROGMEM_STRING(ClearCache, "tspkClear");
PROGMEM_STRING(Address, "tspkAddress");

PROGMEM_STRING(Relay, "tspkRelay");

#if SENSOR_SUPPORT
PROGMEM_STRING(Magnitude, "tspkMagnitude");
#endif

} // namespace keys

namespace {

bool enabled() {
    return getSetting(keys::Enabled, build::enabled());
}

bool clearCache() {
    return getSetting(keys::ClearCache, build::clearCache());
}

espurna::string apiKey() {
    return getSetting(keys::ApiKey, build::ApiKey);
}

espurna::string address() {
    return getSetting(keys::Address, build::Address);
}

#if RELAY_SUPPORT
size_t relay(size_t index) {
    return getSetting({keys::Relay, index}, build::Unset);
}
#endif

#if SENSOR_SUPPORT
size_t magnitude(size_t index) {
    return getSetting({keys::Magnitude, index}, build::Unset);
}
#endif

} // namespace
} // namespace settings

// -----------------------------------------------------------------------------

namespace client {
namespace internal {
namespace {

bool enabled = false;
bool clear = false;

espurna::string fields[build::Fields];

TimeSource::time_point last_flush;
size_t retries = 0;
bool flush = false;

espurna::string data;

} // namespace
} // namespace internal

void schedule_flush() {
    internal::flush = true;
}

void enqueue(size_t index, const espurna::string& payload) {
    if ((index > 0) && (index <= std::size(internal::fields))) {
        internal::fields[--index] = payload;
        return;
    }
}

void value(size_t index, double status) {
    enqueue(index, espurna::string(status, 3));
}

#if RELAY_SUPPORT
bool enqueueRelay(size_t index, const espurna::string& value) {
    if (internal::enabled) {
        auto relayIndex = settings::relay(index);
        if (relayIndex) {
            enqueue(relayIndex, value);
            schedule_flush();
            return true;
        }
    }

    return false;
}

bool enqueueRelay(size_t index, RelayStatus status) {
    return enqueueRelay(index, espurna::string(static_cast<int>(status), 10));
}

bool enqueueRelay(size_t index, bool status) {
    return enqueueRelay(index, espurna::string(static_cast<int>(status), 10));
}

void onRelayStatus(size_t index, bool status) {
    enqueueRelay(index, status);
}
#endif

#if SENSOR_SUPPORT
bool enqueueMagnitude(size_t index, const espurna::string& value) {
    if (internal::enabled) {
        auto magnitudeIndex = settings::magnitude(index);
        if (magnitudeIndex) {
            enqueue(magnitudeIndex, value);
            schedule_flush();
            return true;
        }
    }

    return false;
}
#endif

void maybe_retry(const espurna::string& body) {
    DEBUG_MSG_P(PSTR("[THINGSPEAK] Response: %s\n"), body.c_str());

    if ((!body.length() || body.equals("0")) && (internal::retries < build::Retries)) {
        DEBUG_MSG_P(PSTR("[THINGSPEAK] Re-scheduling flush, attempt %u / %u\n"),
            ++internal::retries, build::Retries);
        schedule_flush();
        return;
    }

    internal::retries = 0;
    if (internal::clear) {
        for (auto& field : internal::fields) {
            field = "";
        }
    }
}

// The POST through the head's one HTTP client (libs/http_client.h): http:// or https:// (the tls module for servers
// that take more than the base suites), the loop drives it
namespace post {
namespace {

// the response: the id of the entry ("0" - not taken), a few bytes
struct Body : public ::espurna::http::Sink {
    const char* begin(const ::espurna::http::Head& head) override {
        text = espurna::string();
        if (head.status != 200) {
            DEBUG_MSG_P(PSTR("[THINGSPEAK] ERROR: HTTP %d\n"), head.status);
        }
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        if (text.length() < 32) {
            text.concat(reinterpret_cast<const char*>(data), std::min<size_t>(len, 32 - text.length()));
        }
        return nullptr;
    }

    const char* end() override {
        return nullptr;
    }

    espurna::string text;
};

::espurna::http::Client client;
Body body;

void done(const ::espurna::http::Result& result) {
    if (!result.ok) {
        DEBUG_MSG_P(PSTR("[THINGSPEAK] ERROR: %s\n"), result.message);
    }

    maybe_retry((result.ok && (result.status == 200)) ? body.text : espurna::string());
}

} // namespace
} // namespace post

bool ready() {
    return !post::client.busy();
}

void send(const espurna::string& address, const espurna::string& data) {
    DEBUG_MSG_P(PSTR("[THINGSPEAK] POST %s?%s\n"), address.c_str(), data.c_str());

    ::espurna::http::Request request;
    request.url = address.c_str();
    request.method = "POST";
    request.type = "application/x-www-form-urlencoded";
    request.body = reinterpret_cast<const uint8_t*>(data.c_str());
    request.body_len = data.length();
    post::client.start(request, post::body, post::done);
}

void flush() {
    static bool initial { true };
    if (!internal::flush) {
        return;
    }

    const auto now = TimeSource::now();
    if (!initial && ((now - internal::last_flush) < build::FlushInterval)) {
        return;
    }

    if (!ready()) {
        return;
    }

    initial = false;
    internal::last_flush = now;
    internal::flush = false;

    internal::data.reserve(build::BufferSize);
    if (internal::data.length()) {
        internal::data = "";
    }

    // Walk the fields, IDs are mapped to indexes of the array
    for (size_t id = 0; id < std::size(internal::fields); ++id) {
        if (internal::fields[id].length()) {
            if (internal::data.length() > 0) {
                internal::data.concat('&');
            }

            char buf[32] = {0};
            snprintf_P(buf, sizeof(buf), PSTR("field%u=%s"),
                (id + 1), internal::fields[id].c_str());
            internal::data.concat(buf);
        }
    }

    // POST data if any
    if (internal::data.length()) {
        internal::data.concat("&api_key=");
        internal::data.concat(settings::apiKey());
        send(settings::address(), internal::data);
    }

    internal::data = "";
}

void configure() {
    internal::enabled = settings::enabled();

    const auto key = settings::apiKey();
    if (internal::enabled && !key.length()) {
        internal::enabled = false;
        setSetting(settings::keys::Enabled, "0");
    }

    internal::clear = settings::clearCache();
}

void loop() {
    post::client.loop();

    if (!internal::enabled) {
        return;
    }

    if (wifiConnected() || wifiConnectable()) {
        flush();
    }
}

} // namespace client

#if WEB_SUPPORT
namespace web {
namespace {

STRING_VIEW_INLINE(Prefix, "tspk");

bool onKeyCheck(StringView key, espurna::json::Value) {
    return key.startsWith(Prefix);
}

void onVisible(espurna::json::Writer& root) {
    bool module { false };
#if RELAY_SUPPORT
    module = module || (relayCount() > 0);
#endif
#if SENSOR_SUPPORT
    module = module || (magnitudeCount() > 0);
#endif
    if (module) {
        wsPayloadModule(root, Prefix);
    }
}

void onConnected(espurna::json::Writer& root) {
    root.field(settings::keys::Enabled, settings::enabled());
    root.field(settings::keys::ApiKey, settings::apiKey());
    root.field(settings::keys::ClearCache, settings::clearCache());
    root.field(settings::keys::Address, settings::address());
#if RELAY_SUPPORT
    root.array(STRING_VIEW("tspkRelays"));
    for (size_t i = 0; i < relayCount(); ++i) {
        root.value(settings::relay(i));
    }
    root.end();
#endif

#if SENSOR_SUPPORT
    sensorWebSocketMagnitudes(root, Prefix, [](espurna::json::Writer& out, size_t index) {
        out.value(settings::magnitude(index));
    });
#endif
}

void setup() {
    wsRegister()
        .onKeyCheck(onKeyCheck)
        .onVisible(onVisible)
        .onConnected(onConnected);
}

} // namespace
} // namespace web
#endif

void setup() {
    client::configure();

#if WEB_SUPPORT
    web::setup();
#endif

#if RELAY_SUPPORT
    relayOnStatusChange(client::onRelayStatus);
    relayOnReady(client::onRelayStatus);
#endif

    espurnaRegisterLoop(client::loop);
    espurnaRegisterReload(client::configure);
}

} // namespace thingspeak
} // namespace espurna

// -----------------------------------------------------------------------------

#if RELAY_SUPPORT
bool tspkEnqueueRelay(size_t index, const espurna::string& value) {
    return ::espurna::thingspeak::client::enqueueRelay(index, value);
}
#endif

#if SENSOR_SUPPORT
bool tspkEnqueueMagnitude(unsigned char index, const espurna::string& value) {
    return ::espurna::thingspeak::client::enqueueMagnitude(index, value);
}
#endif

void tspkFlush() {
    ::espurna::thingspeak::client::schedule_flush();
}

bool tspkEnabled() {
    return ::espurna::thingspeak::client::internal::enabled;
}

void tspkSetup() {
    ::espurna::thingspeak::setup();
}

#endif

