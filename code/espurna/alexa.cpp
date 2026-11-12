/*

ALEXA MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#if ALEXA_SUPPORT

#include <queue>

#include "alexa.h"
#include "api.h"
#include "light.h"
#include "mqtt.h"
#include "relay.h"
#include "rpc.h"
#include "web.h"
#include "ws.h"

#include <fauxmoESP.h>
#include <ArduinoJson.h>

namespace espurna {
namespace alexa {
namespace {

struct Event {
    Event() = delete;
    Event(unsigned char id, bool state, unsigned char value) :
        _id(id),
        _state(state),
        _value(value)
    {}

    unsigned char id() const {
        return _id;
    }

    unsigned char value() const {
        return _value;
    }

    bool state() const {
        return _state;
    }

private:
    unsigned char _id;
    bool _state;
    unsigned char _value;
};

std::queue<Event> events;
fauxmoESP fauxmo;

namespace build {

constexpr uint16_t port() {
    return 80;
}

PROGMEM_STRING(Hostname, ALEXA_HOSTNAME);

constexpr espurna::StringView hostname() {
    return Hostname;
}

constexpr bool enabled() {
    return 1 == ALEXA_ENABLED;
}

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Enabled, "alexaEnabled");
PROGMEM_STRING(Name, "alexaName");

} // namespace keys

bool enabled() {
    return getSetting(keys::Enabled, build::enabled());
}

// Use custom alexa hostname if defined, device hostname otherwise
String hostname() {
    auto out = getSetting(keys::Name, build::hostname());
    if (!out.length()) {
        out = systemHostname();
    }

    return out;
}

void migrate(int version) {
    if (version < 3) {
        moveSetting(PSTR("fauxmoEnabled"), keys::Enabled);
    }
}

} // namespace settings

#if WEB_SUPPORT
namespace web {

STRING_VIEW_INLINE(Prefix, "alexa");

void onVisible(JsonObject& root) {
    wsPayloadModule(root, Prefix);
}

bool onKeyCheck(StringView key, const JsonVariant&) {
    return key.startsWith(Prefix);
}

void onConnected(JsonObject& root) {
    root[FPSTR(settings::keys::Enabled)] = alexa::settings::enabled();
    root[FPSTR(settings::keys::Name)] = alexa::settings::hostname();
}

// fauxmoESP answers through the reply (our fork, 3.4.1-espurna.2: no TCP of its own), the web server sends it
bool process(WebRequest& request, const String& body) {
    return fauxmo.process(request.method() == WebMethod::Get, String(request.url()), body,
        [&request](const char* code, const char* text, const char* mime) {
            request.send(atoi(code), mime, text);
        });
}

// the body of a request no route took, kept until it is complete
struct Body {
    String data;
};

bool body_callback(WebRequest& request, const uint8_t* data, size_t len, size_t index, size_t total) {
    if (total > 1024) {
        DEBUG_MSG_P(PSTR("[ALEXA] Ignoring %s %s from %s (%u bytes)\n"),
            request.methodName(), request.url(),
            ipToString(IPAddress(request.remoteIp())).c_str(), total);
        return false;
    }

    auto* body = static_cast<Body*>(request.context());
    if (!body) {
        body = new (std::nothrow) Body();
        request.setContext(body, [](void* ptr) {
            delete static_cast<Body*>(ptr);
        });
    }

    if (body) {
        if (!index) {
            body->data = String();
        }
        body->data.concat(reinterpret_cast<const char*>(data), len);
    }

    return true;
}

bool request_callback(WebRequest& request) {
    const auto* body = static_cast<Body*>(request.context());
    if (body) {
        return process(request, body->data);
    }

    const char* form = request.param("body");
    return process(request, form ? String(form) : String());
}

void setup() {
    webBodyRegister(body_callback);
    webRequestRegister(request_callback);
    wsRegister()
        .onVisible(onVisible)
        .onConnected(onConnected)
        .onKeyCheck(onKeyCheck);
}

} // namespace web
#endif

// Matter commissioning pauses the fauxmo servers (UDP + TCP) to leave the heap to the certificate checks
bool paused { false };

void configure() {
    fauxmo.enable(wifiConnected() && alexa::settings::enabled() && !paused);
}

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE

void update() {
    fauxmo.setState(uint8_t{ 0 }, lightState(), lightState() ? 255u : 0u);

    auto channels = lightChannels();
    for (decltype(channels) channel = 0; channel < channels; ++channel) {
        auto value = lightChannel(channel);
        fauxmo.setState(channel + 1, value > 0, value);
    }
}

#elif RELAY_SUPPORT

void update(size_t id, bool status) {
    fauxmo.setState(id, status, status ? 255 : 0);
}

#endif

void loop() {
    fauxmo.handle();

    while (!events.empty()) {
        auto& event = events.front();
        DEBUG_MSG_P(PSTR("[ALEXA] Device #%hhu state=#%c value=%hhu\n"),
            event.id(), event.state() ? 't' : 'f', event.value());

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
        if (0 == event.id()) {
            lightState(event.state());
        } else {
            lightState(event.id() - 1, event.state());
            lightChannel(event.id() - 1, event.value());
            lightUpdate();
        }
#else
        relayStatus(event.id(), event.state());
#endif

        events.pop();
    }
}

void setup() {
    // Backwards compatibility
    migrateVersion(settings::migrate);

    // Basic fauxmoESP configuration
    fauxmo.setPort(alexa::build::port());

    auto hostname = alexa::settings::hostname();
    auto deviceName = [&](size_t index) {
        auto name = hostname;
        name += ' ';
        name += index;
        return name;
    };

#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
    // 1st is the global state, the rest are mapped to channel values
    fauxmo.addDevice(hostname.c_str());
    for (size_t channel = 1; channel <= lightChannels(); ++channel) {
        fauxmo.addDevice(deviceName(channel).c_str());
    }

    // Relays are mapped 1-to-1
#elif RELAY_SUPPORT
    auto relays = relayCount();
    if (relays > 1) {
        for (decltype(relays) id = 1; id <= relays; ++id) {
            fauxmo.addDevice(deviceName(id).c_str());
        }
    } else {
        fauxmo.addDevice(hostname.c_str());
    }
#endif

#if WEB_SUPPORT
    web::setup();
#endif

    // Register wifi callback
    wifiRegister([](espurna::wifi::Event event) {
        switch (event) {
        case espurna::wifi::Event::StationConnected:
        case espurna::wifi::Event::StationDisconnected:
            configure();
        default:
            break;
        }
    });

    // Callback
    fauxmo.onSetState([](unsigned char device_id, const char*, bool state, unsigned char value) {
        events.emplace(device_id, state, value);
    });

    // Register main callbacks
#if LIGHT_PROVIDER != LIGHT_PROVIDER_NONE
    lightOnReport(update);
#elif RELAY_SUPPORT
    relayOnStatusChange(update);
#endif

    espurnaRegisterReload(configure);
    espurnaRegisterLoop(loop);
}

} // namespace
} // namespace alexa
} // namespace espurna

bool alexaEnabled() {
    return espurna::alexa::settings::enabled();
}

void alexaPause(bool paused) {
    espurna::alexa::paused = paused;
    espurna::alexa::configure();
}

void alexaSetup() {
    espurna::alexa::setup();
}

#endif

