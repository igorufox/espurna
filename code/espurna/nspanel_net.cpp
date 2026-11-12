/*

NSPANEL MODULE - network

MQTT subscriptions of the entities and their state payloads (plain and JSON), the history series of
the charts, the Open-Meteo weather and the GET requests of "url" tiles (the head's one asynchronous HTTP client,
libs/http_client.h; the tls module it needs gives it ECDHE / ECDSA for these servers)

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "espurna_tasks.h"
#include "nspanel_core.h"

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#include <ArduinoJson.h>

#include "libs/http_client.h"

#include <cmath>
#include <cstdarg>
#include <memory>
#include <new>
#include <vector>

namespace espurna {
namespace nspanel {

// -----------------------------------------------------------------------------
// "url" tiles: an HTTP GET per tap through the head's one asynchronous client (a slow or dead server stalls
// nothing, the loop drives it); the result is logged, never the URL itself (it may hold a token)
// -----------------------------------------------------------------------------

namespace http {
namespace {

constexpr size_t RunningMax { 2 };

struct Request {
    ::espurna::http::Client client;
    ::espurna::http::Discard sink;
};

Request requests[RunningMax];

void done(const ::espurna::http::Result& result) {
    if (result.status > 0) {
        DEBUG_MSG_P(PSTR("[NSPANEL] URL request: HTTP %d\n"), result.status);
    } else {
        DEBUG_MSG_P(PSTR("[NSPANEL] URL request failed: %s\n"), result.message);
    }
}

} // namespace

// false when it could not be started (no WiFi, too many at once, low heap)
bool get(const String& url) {
    if (wifiConnected() && (systemFreeHeap() >= build::WeatherHeapMin)) {
        for (auto& request : requests) {
            if (!request.client.busy()) {
                return request.client.start(url.c_str(), nullptr, request.sink, done);
            }
        }
    }

    DEBUG_MSG_P(PSTR("[NSPANEL] URL request dropped (no WiFi, busy or low heap)\n"));
    return false;
}

void loop() {
    for (auto& request : requests) {
        request.client.loop();
    }
}

} // namespace http

// -----------------------------------------------------------------------------
// Weather (Open-Meteo) through the head's one asynchronous client, driven from the loop
// -----------------------------------------------------------------------------

namespace weather {
namespace {

bool running_now { false };
Data data;
char url[384];
uint32_t next_ms { 15000 };
uint32_t failures { 0 };
char last_error[64] { "never" };

void error(const char* format, ...) __attribute__((format(printf, 1, 2)));
void error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
}

String lastError() {
    return String(last_error);
}

// the response body, at most WeatherBodyMax bytes (the client decodes a chunked body)
struct Body : public ::espurna::http::Sink {
    const char* begin(const ::espurna::http::Head& head) override {
        text = String();
        if (head.status != 200) {
            return PSTR("HTTP status not 200");
        }
        if (head.length > static_cast<long>(build::WeatherBodyMax)) {
            return PSTR("response too large");
        }
        text.reserve((head.length > 0) ? head.length : 2048);
        return nullptr;
    }

    const char* write(const uint8_t* bytes, size_t size) override {
        if ((text.length() + size) > build::WeatherBodyMax) {
            return PSTR("response too large");
        }
        text.concat(reinterpret_cast<const char*>(bytes), size);
        return nullptr;
    }

    const char* end() override {
        return nullptr;
    }

    String text;
};

::espurna::http::Client client;
Body body;

// the JSON of Open-Meteo into `out`; false - not a forecast
bool parse(const String& text, Data& out) {
    {
        DynamicJsonBuffer buffer(1024);
        JsonObject& root = buffer.parseObject(text.c_str());
        JsonObject& cur = root["current"];
        JsonObject& daily = root["daily"];
        if (root.success() && cur.success()) {
            out.temperature = cur["temperature_2m"].as<float>();
            out.code = cur["weather_code"] | -1;
            out.day = (cur["is_day"] | 1) != 0;
            out.valid = true;

            if (daily.success()) {
                JsonArray& times = daily["time"];
                JsonArray& codes = daily["weather_code"];
                JsonArray& tmax = daily["temperature_2m_max"];
                JsonArray& tmin = daily["temperature_2m_min"];
                JsonArray& wind = daily["wind_speed_10m_max"];
                JsonArray& rain = daily["precipitation_probability_max"];
                JsonArray& uv = daily["uv_index_max"];
                const size_t count = std::min<size_t>(DaysMax, times.size());
                for (size_t n = 0; n < count; ++n) {
                    int y = 0, m = 0, d = 0;
                    const char* date = times[n].as<const char*>();
                    if (!date || (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3)) {
                        break;
                    }
                    // month 1..12, else no weekday (and no index into the month names)
                    const int wday = weekday(y, m, d);
                    if ((wday < 0) || (d < 1) || (d > 31)) {
                        break;
                    }
                    out.days[n].wday = wday;
                    out.days[n].mday = d;
                    out.days[n].mon = m - 1;
                    out.days[n].code = codes[n] | -1;
                    out.days[n].tmax = tmax[n].as<float>();
                    out.days[n].tmin = tmin[n].as<float>();
                    if (wind.success() && wind[n].is<float>()) {
                        out.days[n].wind = wind[n].as<float>();
                    }
                    if (rain.success() && rain[n].is<int>()) {
                        out.days[n].rain = rain[n].as<int>();
                    }
                    if (uv.success() && uv[n].is<float>()) {
                        out.days[n].uv = uv[n].as<float>();
                    }
                    out.count = n + 1;
                }
            }
        }
    }

    return out.valid;
}

void done(const ::espurna::http::Result& result) {
    running_now = false;
    const auto now = millis();

    Data out;
    if (!result.ok) {
        error("%s", result.message);
    } else if (!parse(body.text, out)) {
        error("invalid response (%u bytes)", body.text.length());
    } else {
        out.updated = ::time(nullptr);
        data = out;
        body.text = String();
        error("ok");
        failures = 0;
        next_ms = now + config().refresh * 1000ul;
        DEBUG_MSG_P(PSTR("[NSPANEL] Weather: %.1fC code %d, %u days\n"),
            data.temperature, data.code, data.count);
        driver::weatherChanged();
        return;
    }

    body.text = String();
    ++failures;
    next_ms = now + build::WeatherRetryMs;
    DEBUG_MSG_P(PSTR("[NSPANEL] Weather update failed: %s\n"), last_error);
}

} // namespace

const Data& current() {
    return data;
}

void reset() {
    data = Data();
    next_ms = millis();
}

// a request holds a TLS session, config parsing waits for it (ref. upload::process())
bool running() {
    return running_now;
}

void info(Print& out) {
    out.printf_P(PSTR("Weather: %s, last: %s, %.1fC code %d, %u days\n"),
        config().weather ? "enabled" : "disabled", lastError().c_str(),
        data.temperature, data.code, data.count);
}

void loop() {
    client.loop();

    const auto& cfg = config();
    if (!cfg.weather || running_now) {
        return;
    }

    const auto now = millis();
    if (static_cast<int32_t>(now - next_ms) < 0) {
        return;
    }

    if (!wifiConnected()) {
        next_ms = now + 5000;
        return;
    }

    // a config upload being received / parsed goes first, ref. upload::process()
    if (upload::busy()) {
        next_ms = now + 2000;
        return;
    }

    const auto heap = systemFreeHeap();
    if (heap < build::WeatherHeapMin) {
        error("low heap (%u)", static_cast<unsigned>(heap));
        next_ms = now + build::WeatherRetryMs;
        return;
    }

    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,is_day"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
        "wind_speed_10m_max,precipitation_probability_max,uv_index_max"
        "&wind_speed_unit=ms&timezone=auto&forecast_days=7",
        cfg.lat, cfg.lon);

    // a failure the client reports at once already went through done()
    running_now = true;
    if (!client.start(url, nullptr, body, done) && running_now) {
        running_now = false;
        error("cannot start the request");
        next_ms = now + build::WeatherRetryMs;
    }
}

} // namespace weather

// -----------------------------------------------------------------------------
// JSON state payloads. The MQTT callback (AsyncTCP task, under the loop lock) only keeps the
// latest payload per topic; loop() parses one per call (ref. json::loop()).
// -----------------------------------------------------------------------------

namespace json {
namespace {

constexpr size_t PendingMax { 8 };

struct Pending {
    String topic;
    String payload;
};

std::vector<Pending> pending;
uint32_t parsed { 0 };
uint32_t failed { 0 };
uint32_t dropped { 0 };

void queue(espurna::StringView topic, espurna::StringView payload) {
    for (auto& entry : pending) {
        if (topic.equals(StringView(entry.topic))) {
            entry.payload = payload.toString();
            return;
        }
    }

    if (pending.size() >= PendingMax) {
        pending.erase(pending.begin());
        ++dropped;
    }

    Pending entry;
    entry.topic = topic.toString();
    entry.payload = payload.toString();
    pending.push_back(std::move(entry));
}

// the value at the entity's path: text and number
bool extract(JsonVariant root, const String& path, String& text, float& number) {
    JsonVariant value = root;
    int start = 0;
    while (start <= static_cast<int>(path.length())) {
        int end = path.indexOf(PathSep, start);
        if (end < 0) {
            end = path.length();
        }
        const String key = path.substring(start, end);
        start = end + 1;

        if (value.is<JsonObject>()) {
            JsonObject& obj = value.as<JsonObject>();
            if (!obj.containsKey(key)) {
                return false;
            }
            value = obj.get<JsonVariant>(key);
        } else if (value.is<JsonArray>()) {
            JsonArray& array = value.as<JsonArray>();
            const long n = key.toInt();
            if ((n < 0) || (static_cast<size_t>(n) >= array.size())) {
                return false;
            }
            value = array.get<JsonVariant>(n);
        } else {
            return false;
        }
    }

    number = NAN;
    if (value.is<const char*>()) {
        // ArduinoJson 5 takes a JSON null for a string and gives NULL for it: a String assigned NULL has no
        // buffer, and strtof(NULL) below crashed the panel on any null at an entity's path (crash loop -> unstable mode)
        const char* raw = value.as<const char*>();
        if (!raw) {
            return false;
        }
        text = raw;
        char* end = nullptr;
        const float parsed = strtof(text.c_str(), &end);
        if (text.length() && end && !*end) {
            number = parsed;
        }
    } else if (value.is<bool>()) {
        text = value.as<bool>() ? F("true") : F("false");
    } else if (value.is<long>() || value.is<double>()) {
        number = value.as<float>();
        char buf[24];
        snprintf(buf, sizeof(buf), "%g", value.as<double>());
        text = buf;
    } else {
        return false; // object, array or null
    }

    utf8Truncate(text, 32);
    return true;
}

void process(Pending& entry) {
    DynamicJsonBuffer buffer(512);
    // parses in place: the strings stay in entry.payload, nothing is copied
    JsonVariant root = buffer.parse(entry.payload.begin());
    if (!root.is<JsonObject>() && !root.is<JsonArray>()) {
        ++failed;
        return;
    }
    ++parsed;

    auto& cfg = config();
    for (size_t index = 0; index < cfg.entities.size(); ++index) {
        auto& entity = cfg.entities[index];
        if (!entity.path.length() || (entity.topic != entry.topic)) {
            continue;
        }

        String text;
        float number = NAN;
        if (!extract(root, entity.path, text, number)) {
            continue; // not in this message (z2m sends partial updates too)
        }

        const String& on = entity.on.length() ? entity.on : cfg.on;
        const String& off = entity.off.length() ? entity.off : cfg.off;
        const int state = (text == on) ? 1 : (text == off) ? 0 : -1;

        const bool same_number = (std::isnan(number) && std::isnan(entity.number)) || (number == entity.number);
        if ((state != entity.state) || (text != entity.value) || !same_number) {
            entity.state = state;
            entity.value = std::move(text);
            entity.number = number;
            entityChanged(index);
        }
    }
}

void loop() {
    if (pending.empty()) {
        return;
    }

    Pending entry = std::move(pending.front());
    pending.erase(pending.begin());
    process(entry);
}

} // namespace

Stats stats() {
    return Stats{parsed, failed, dropped, pending.size()};
}

} // namespace json

// -----------------------------------------------------------------------------
// History: a thermostat's or a switch tile's "history" topic holds a retained series (e.g. published by an openHAB rule
// from its persistence). It is subscribed only while the chart is open, so nothing is kept on the panel:
// {"from": <unix time of the first point>, "step": <seconds>, "t": [<temperature x10> | null, ...], "s": [<setpoint x10> | null, ...],
//  "h": [<humidity % x10> | null, ...]} ("h" is optional), or for a switch {"from", "step", "p": [<% of the step it was ON> | null, ...]}
// -----------------------------------------------------------------------------

namespace history {
namespace {

String topic;               // subscribed while a chart is open, empty otherwise
String payload;             // the last message, parsed in loop()
bool is_fresh { false };
bool subscribed { false };

// loop(): subscribe again after a reconnect (or when the first attempt found no room)
void loop() {
#if MQTT_SUPPORT
    if (topic.length() && !subscribed && mqttConnected()) {
        subscribed = mqttSubscribeRaw(topic.c_str());
    }
#endif
}

// MQTT callback: only keep the text
void message(StringView name, StringView body) {
    if (topic.length() && name.equals(StringView(topic)) && (body.length() < PayloadMax)) {
        payload = body.toString();
        is_fresh = true;
    }
}

void read(JsonVariant values, std::vector<int16_t>& out) {
    out.clear();
    JsonArray& array = values;
    if (!array.success()) {
        return;
    }
    out.reserve(std::min(array.size(), PointsMax));
    for (auto value : array) {
        if (out.size() >= PointsMax) {
            break;
        }
        if (value.is<long>() || value.is<double>()) {
            const long number = lroundf(value.as<float>());
            out.push_back(static_cast<int16_t>(std::max(-32000L, std::min(32000L, number))));
        } else {
            out.push_back(Missing);
        }
    }
}

} // namespace

void close() {
#if MQTT_SUPPORT
    if (subscribed && mqttConnected()) {
        mqttUnsubscribeRaw(topic.c_str());
    }
#endif
    topic = String();
    payload = String();
    is_fresh = false;
    subscribed = false;
}

// the broker sends the retained message right after the subscription
void open(const String& name) {
    close();
    topic = name;
#if MQTT_SUPPORT
    subscribed = mqttConnected() && mqttSubscribeRaw(topic.c_str());
#endif
}

bool fresh() {
    return is_fresh;
}

// a new message, parsed into out; false when there is none or it is not a series
bool take(Series& out) {
    if (!is_fresh) {
        return false;
    }
    is_fresh = false;

    try {
        DynamicJsonBuffer buffer(4096);
        JsonObject& root = buffer.parseObject(payload.c_str());
        if (!root.success()) {
            return false;
        }
        out.from = root["from"].as<unsigned long>();
        out.step = root["step"].as<unsigned long>();
        read(root["t"], out.temperature);
        read(root["s"], out.setpoint);
        read(root["h"], out.humidity);
        read(root["p"], out.on);
    } catch (const std::bad_alloc&) {
        return false;
    }

    return (out.step > 0) && ((out.temperature.size() > 1) || (out.on.size() > 1));
}

} // namespace history

// -----------------------------------------------------------------------------
// MQTT subscriptions of the entities
// -----------------------------------------------------------------------------

namespace {

std::vector<String> subscribed;

// On ESP32 the MQTT connect event usually runs in the async_tcp task. Every subscribe() there
// is a blocking tcpip_api_call(), while each SUBACK / retained message / ACK the broker sends back
// is queued by the tcpip thread into the 32-slot async_tcp event queue with portMAX_DELAY.
// Two dozen subscriptions in one callback can fill that queue: the tcpip thread then waits for
// async_tcp, async_tcp waits for the tcpip thread, and the Task Watchdog resets the device
// (async_tcp is watched while it handles an event, loopTask waits for the loop lock it holds).
// So the subscriptions are only scheduled here and sent from loop(), see subscribeLoop().
size_t subscribe_next { 0 };
bool subscribe_pending { false };
bool announce_pending { false };
uint32_t subscribe_last { 0 };

void subscribeLoop() {
#if MQTT_SUPPORT
    if (!subscribe_pending && !announce_pending) {
        return;
    }

    // MQTT_CONNECT_EVENT schedules everything again
    if (!mqttConnected()) {
        return;
    }

    const auto now = millis();
    if (now - subscribe_last < build::SubscribeStepMs) {
        return;
    }
    subscribe_last = now;

    const auto& cfg = config();
    size_t budget = build::SubscribeBurst;
    while (subscribe_pending && budget) {
        if (subscribe_next >= cfg.entities.size()) {
            subscribe_pending = false;
            break;
        }

        const auto& entity = cfg.entities[subscribe_next];
        bool known = false; // several JSON entities share a topic: subscribe once
        for (const auto& topic : subscribed) {
            if (topic == entity.topic) {
                known = true;
                break;
            }
        }
        if (entity.topic.length() && !known) {
            // 0: no room in the TCP send buffer right now, retry on the next step
            if (!mqttSubscribeRaw(entity.topic.c_str())) {
                return;
            }
            subscribed.push_back(entity.topic);
            --budget;
        }
        ++subscribe_next;
    }

    if (!subscribe_pending && announce_pending) {
        announce_pending = false;
        const auto& info = panelInfo();
        if (info.length()) {
            mqttSendRaw(mqttTopic("nspanel").c_str(), info.c_str(), false);
        }
    }
#endif
}

#if MQTT_SUPPORT
void mqttCallback(unsigned int type, StringView topic, StringView payload) {
    switch (type) {
    case MQTT_CONNECT_EVENT:
        // nothing is sent from here, ref. subscribeAll()
        subscribeAll();
        announce_pending = true;
        history::subscribed = false;
        break;

    case MQTT_DISCONNECT_EVENT:
        subscribed.clear();
        subscribe_pending = false;
        announce_pending = false;
        history::subscribed = false;
        break;

    case MQTT_MESSAGE_EVENT: {
        history::message(topic, payload);

        auto& cfg = config();
        bool parse = false;
        for (size_t index = 0; index < cfg.entities.size(); ++index) {
            auto& entity = cfg.entities[index];
            if (!entity.topic.length() || !topic.equals(StringView(entity.topic))) {
                continue;
            }

            // JSON payloads are parsed in loop(), nothing heavy here
            if (entity.path.length()) {
                parse = true;
                continue;
            }

            const String& on = entity.on.length() ? entity.on : cfg.on;
            const String& off = entity.off.length() ? entity.off : cfg.off;
            const int state = payload.equals(StringView(on)) ? 1
                : payload.equals(StringView(off)) ? 0
                : -1;

            String value = payload.toString();
            utf8Truncate(value, 32);

            char* end = nullptr;
            const float number = strtof(value.c_str(), &end);
            entity.number = (value.length() && end && !*end) ? number : NAN;

            if ((state != entity.state) || (value != entity.value)) {
                entity.state = state;
                entity.value = std::move(value);
                entityChanged(index);
            }
        }

        if (parse) {
            json::queue(topic, payload);
        }
        break;
    }
    }
}
#endif

} // namespace

void unsubscribeAll() {
#if MQTT_SUPPORT
    if (mqttConnected()) {
        for (const auto& topic : subscribed) {
            mqttUnsubscribeRaw(topic.c_str());
        }
    }
#endif
    subscribed.clear();
}

void subscribeAll() {
    subscribed.clear();
    subscribe_next = 0;
    subscribe_pending = true;
}

size_t subscribedCount() {
    return subscribed.size();
}

void setupNet() {
#if MQTT_SUPPORT
    mqttRegister(mqttCallback);
#endif
}

// before the display: subscriptions, the open chart's series, one JSON state payload
void netLoop() {
    subscribeLoop();
    history::loop();
    json::loop();
}

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_SUPPORT
