/*

NSPANEL MODULE

Sonoff NSPanel (gen1 EU, ESP32) backend for the joBr99 "NSPanel Lovelace UI" TFT.
Protocol reference: nspanel/lovelace-protocol.md (55 BB framing, CRC16/MODBUS, '~' commands / ',' events)

- UART2 (TX GPIO16, RX GPIO17, 115200) driver with a paced TX queue and a byte-wise RX parser
- screensaver (time, Russian date, Open-Meteo weather, NTC temperature) and grid cards built from /nspanel.json
- MQTT state map (absolute state topics), local relays, hardware button custom actions
- LittleFS on the existing data/spiffs partition (Tasmota layout), /nspanel and /nspanel.json web endpoints
- Tasmota safeboot layout support: `safeboot` command, crash-loop guard, web OTA guard

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "nspanel.h"
#include "nspanel_html.h"
#include "nspanel_icons.h"

#if BUTTON_SUPPORT
#include "button.h"
#endif

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#if RELAY_SUPPORT
#include "relay.h"
#endif

#if WEB_SUPPORT
#include "web.h"
#endif

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <atomic>
#include <cmath>
#include <ctime>
#include <deque>
#include <memory>
#include <new>
#include <vector>

namespace espurna {
namespace nspanel {
namespace {

namespace build {

constexpr int DisplayPowerPin { 4 };      // LOW = display ON
constexpr int TxPin { 16 };
constexpr int RxPin { 17 };
constexpr uint32_t Baudrate { 115200 };
constexpr int BuzzerPin { 21 };
constexpr int BuzzerChannel { 15 };       // pwm.cpp allocates LEDC channels from 0
constexpr int NtcPin { 38 };

constexpr size_t FrameMax { 1024 };
constexpr size_t QueueMax { 48 };
constexpr uint32_t TxGapMs { 75 };
constexpr uint32_t StartupRetryMs { 10000 };
constexpr uint32_t RenderDelayMs { 100 };
constexpr size_t EventsMax { 8 };

constexpr size_t ConfigMax { 16384 };
constexpr char ConfigPath[] = "/nspanel.json";
constexpr char ConfigTmpPath[] = "/nspanel.json.tmp";

constexpr uint32_t CrashLoopMax { 3 };
constexpr uint32_t CrashLoopUptimeMs { 60000 };

// Tasmota NSPanel template: ADCParam1 2,11200,10000,3950 (NTC to GND, 11.2k pull-up to 3.3V)
// ESPHome NSPanel configs use the same values (resistance DOWNSTREAM 11.2kOhm, B 3950, 10kOhm @ 25C)
constexpr float NtcPullup { 11200.0f };
constexpr float NtcR0 { 10000.0f };
constexpr float NtcBeta { 3950.0f };
constexpr float NtcT0 { 298.15f };
constexpr float NtcSupplyMv { 3300.0f };
constexpr uint32_t NtcReadMs { 10000 };

constexpr uint32_t WeatherStack { 12288 };
constexpr size_t WeatherHeapMin { 50000 };
constexpr uint32_t WeatherRetryMs { 60000 };

// TFT text limits, bytes (Max Text Size of the 51 HMI)
constexpr size_t HeadingMax { 40 };
constexpr size_t NameMax { 30 };
constexpr size_t DateMax { 40 };
constexpr size_t ForecastMax { 10 };

} // namespace build

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

String utf8(uint16_t code) {
    String out;
    if (!code) {
        return out;
    }

    char buf[4];
    if (code < 0x80) {
        buf[0] = static_cast<char>(code);
        buf[1] = '\0';
    } else if (code < 0x800) {
        buf[0] = static_cast<char>(0xC0 | (code >> 6));
        buf[1] = static_cast<char>(0x80 | (code & 0x3F));
        buf[2] = '\0';
    } else {
        buf[0] = static_cast<char>(0xE0 | (code >> 12));
        buf[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (code & 0x3F));
        buf[3] = '\0';
    }

    out = buf;
    return out;
}

// cut at 'max' bytes without splitting a multi-byte sequence
void utf8Truncate(String& value, size_t max) {
    if (value.length() <= max) {
        return;
    }

    size_t len = max;
    while ((len > 0) && ((static_cast<uint8_t>(value[len]) & 0xC0) == 0x80)) {
        --len;
    }

    value.remove(len);
}

// first 'chars' UTF-8 characters
String utf8Prefix(const String& value, size_t chars) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < value.length()) {
        if ((static_cast<uint8_t>(value[pos]) & 0xC0) != 0x80) {
            if (count == chars) {
                break;
            }
            ++count;
        }
        ++pos;
    }

    return value.substring(0, pos);
}

// '~' separates fields of ESP->TFT commands
String sanitize(const char* value, size_t max) {
    String out(value ? value : "");
    out.replace("~", "-");
    out.replace("\r", "");
    out.replace("\n", " ");
    utf8Truncate(out, max);
    return out;
}

uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }

    return crc;
}

// Sakamoto, 0 = Sunday
int weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

namespace ru {

const char* const WeekdayFull[] {
    "Воскресенье", "Понедельник", "Вторник", "Среда", "Четверг", "Пятница", "Суббота",
};

const char* const WeekdayShort[] {
    "Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб",
};

const char* const MonthGenitive[] {
    "января", "февраля", "марта", "апреля", "мая", "июня",
    "июля", "августа", "сентября", "октября", "ноября", "декабря",
};

String date(const tm& t) {
    const int wday = ((t.tm_wday % 7) + 7) % 7;
    const int mon = ((t.tm_mon % 12) + 12) % 12;
    const int year = t.tm_year + 1900;

    char buf[96];
    snprintf(buf, sizeof(buf), "%s, %d %s %d", WeekdayFull[wday], t.tm_mday, MonthGenitive[mon], year);
    if (strlen(buf) <= build::DateMax) {
        return buf;
    }

    snprintf(buf, sizeof(buf), "%s, %d %s", WeekdayFull[wday], t.tm_mday, MonthGenitive[mon]);
    if (strlen(buf) <= build::DateMax) {
        return buf;
    }

    snprintf(buf, sizeof(buf), "%s, %d %s %d", WeekdayShort[wday], t.tm_mday, MonthGenitive[mon], year);
    return buf;
}

} // namespace ru

// -----------------------------------------------------------------------------
// Config model
// -----------------------------------------------------------------------------

enum class Action : uint8_t {
    Toggle,
    On,
    Off,
};

enum class ItemType : uint8_t {
    Switch,
    Button,
    Scene,
    Text,
};

struct Entity {
    String topic;    // absolute MQTT state topic, empty for local relays
    String command;  // absolute MQTT command topic
    int relay { -1 };
    int state { -1 }; // -1 unknown, 0 off, 1 on
    String value;
};

struct Target {
    int entity { -1 };
    int group { -1 };
    int page { -1 };
    Action action { Action::Toggle };
    bool valid { false };
};

struct Item {
    String name;
    String icon;
    ItemType type { ItemType::Switch };
    Target target;
};

struct Page {
    String title;
    std::vector<uint16_t> items;
};

struct Config {
    String on { "1" };
    String off { "0" };
    String toggle { "2" };
    String set { "/set" };

    uint16_t timeout { 20 };
    uint8_t brightness { 100 };
    uint8_t dim { 20 };

    bool weather { false };
    float lat { 0.0f };
    float lon { 0.0f };
    uint32_t refresh { 900 };

    bool temperature { true };
    bool temperature_show { true };
    float temperature_offset { 0.0f };
    uint32_t temperature_publish { 60 };

    bool beep { false };
    bool debug { false };

    uint16_t color_on { icons::colors::On };
    uint16_t color_off { icons::colors::Off };
    uint16_t color_unknown { icons::colors::Unknown };

    std::vector<Entity> entities;
    std::vector<String> group_names;
    std::vector<std::vector<int>> groups;
    std::vector<Item> items;
    std::vector<Page> pages;
    std::vector<Target> buttons;
};

Config config;

int findOrAddTopic(Config& out, const String& topic, const String& command) {
    for (size_t index = 0; index < out.entities.size(); ++index) {
        if (out.entities[index].topic == topic) {
            if (command.length()) {
                out.entities[index].command = command;
            }
            return index;
        }
    }

    Entity entity;
    entity.topic = topic;
    entity.command = command.length() ? command : (topic + out.set);
    out.entities.push_back(std::move(entity));

    return out.entities.size() - 1;
}

int findOrAddRelay(Config& out, int relay) {
    for (size_t index = 0; index < out.entities.size(); ++index) {
        if (out.entities[index].relay == relay) {
            return index;
        }
    }

    Entity entity;
    entity.relay = relay;
    out.entities.push_back(std::move(entity));

    return out.entities.size() - 1;
}

bool parseAction(const char* value, Action& out) {
    if (!value || !strlen(value) || (0 == strcmp(value, "toggle"))) {
        out = Action::Toggle;
    } else if (0 == strcmp(value, "on")) {
        out = Action::On;
    } else if (0 == strcmp(value, "off")) {
        out = Action::Off;
    } else {
        return false;
    }

    return true;
}

bool parseHex(const char* value, uint16_t& out) {
    if (!value) {
        return false;
    }

    if ((value[0] == 'U' || value[0] == 'u') && value[1] == '+') {
        value += 2;
    } else if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value += 2;
    }

    if (strlen(value) != 4) {
        return false;
    }

    char* end = nullptr;
    const auto result = strtoul(value, &end, 16);
    if (!end || *end != '\0' || result < 0xE000) {
        return false;
    }

    out = result;
    return true;
}

String resolveIcon(const char* value, uint16_t fallback, String& warnings) {
    if (!value || !strlen(value)) {
        return utf8(fallback);
    }

    // literal glyph or short text
    if (static_cast<uint8_t>(value[0]) >= 0x80) {
        String out(value);
        utf8Truncate(out, 9);
        return out;
    }

    const char* name = value;
    if (0 == strncmp(name, "mdi:", 4)) {
        name += 4;
    }

    for (const auto& icon : icons::Table) {
        if (0 == strcmp(icon.name, name)) {
            return utf8(icon.code);
        }
    }

    uint16_t code;
    if (parseHex(name, code)) {
        return utf8(code);
    }

    warnings += F("unknown icon '");
    warnings += value;
    warnings += F("'; ");

    return utf8(icons::AlertCircleOutline);
}

bool parseTarget(JsonObject& obj, Config& out, Target& target, String& error) {
    if (!parseAction(obj["action"].as<const char*>(), target.action)) {
        error = F("unknown action '");
        error += obj["action"].as<const char*>();
        error += '\'';
        return false;
    }

    if (obj.containsKey("relay")) {
        const int relay = obj["relay"].as<int>();
#if RELAY_SUPPORT
        if ((relay < 0) || (static_cast<size_t>(relay) >= relayCount())) {
#else
        if (true) {
#endif
            error = F("no such relay ");
            error += relay;
            return false;
        }
        target.entity = findOrAddRelay(out, relay);
    } else if (obj.containsKey("topic")) {
        const char* topic = obj["topic"].as<const char*>();
        if (!topic || !strlen(topic) || strchr(topic, '#') || strchr(topic, '+')) {
            error = F("invalid topic");
            return false;
        }

        const char* command = obj["command"].as<const char*>();
        target.entity = findOrAddTopic(out, topic, command ? String(command) : String());
    }

    if (obj.containsKey("group")) {
        const char* name = obj["group"].as<const char*>();
        for (size_t index = 0; index < out.group_names.size(); ++index) {
            if (name && out.group_names[index] == name) {
                target.group = index;
                break;
            }
        }

        if (target.group < 0) {
            error = F("unknown group '");
            error += name ? name : "";
            error += '\'';
            return false;
        }
    }

    if (obj.containsKey("page")) {
        target.page = obj["page"].as<int>();
    }

    target.valid = (target.entity >= 0) || (target.group >= 0) || (target.page >= 0);
    return true;
}

template <typename T>
T clampValue(long value, long min, long max) {
    return static_cast<T>(std::max(min, std::min(max, value)));
}

bool parse(const char* text, Config& out, String& error, String& warnings) {
    DynamicJsonBuffer buffer(1024);
    JsonObject& root = buffer.parseObject(text);
    if (!root.success()) {
        error = F("invalid JSON");
        return false;
    }

    JsonObject& mqtt = root["mqtt"];
    if (mqtt.success()) {
        if (mqtt.containsKey("on")) out.on = mqtt["on"].as<const char*>();
        if (mqtt.containsKey("off")) out.off = mqtt["off"].as<const char*>();
        if (mqtt.containsKey("toggle")) out.toggle = mqtt["toggle"].as<const char*>();
        if (mqtt.containsKey("set")) out.set = mqtt["set"].as<const char*>();
    }

    JsonObject& screensaver = root["screensaver"];
    if (screensaver.success()) {
        if (screensaver.containsKey("timeout")) {
            out.timeout = clampValue<uint16_t>(screensaver["timeout"].as<long>(), 0, 3600);
        }
        if (screensaver.containsKey("brightness")) {
            out.brightness = clampValue<uint8_t>(screensaver["brightness"].as<long>(), 1, 100);
        }
        if (screensaver.containsKey("dim")) {
            out.dim = clampValue<uint8_t>(screensaver["dim"].as<long>(), 0, 100);
        }

        JsonObject& weather = screensaver["weather"];
        if (weather.success() && weather.containsKey("lat") && weather.containsKey("lon")) {
            out.weather = weather["enabled"] | true;
            out.lat = weather["lat"].as<float>();
            out.lon = weather["lon"].as<float>();
            if (weather.containsKey("refresh")) {
                out.refresh = clampValue<uint32_t>(weather["refresh"].as<long>(), 60, 86400);
            }
        }
    }

    // TFT sleep timer only runs when dim != brightness
    if (out.dim == out.brightness) {
        out.dim = out.brightness - 1;
    }

    JsonObject& temperature = root["temperature"];
    if (temperature.success()) {
        out.temperature = temperature["enabled"] | true;
        out.temperature_show = temperature["show"] | true;
        out.temperature_offset = temperature["offset"] | 0.0f;
        if (temperature.containsKey("publish")) {
            out.temperature_publish = clampValue<uint32_t>(temperature["publish"].as<long>(), 0, 86400);
        }
    }

    JsonObject& colors = root["colors"];
    if (colors.success()) {
        if (colors.containsKey("on")) out.color_on = colors["on"].as<unsigned int>();
        if (colors.containsKey("off")) out.color_off = colors["off"].as<unsigned int>();
        if (colors.containsKey("unknown")) out.color_unknown = colors["unknown"].as<unsigned int>();
    }

    out.beep = root["beep"] | false;
    out.debug = root["debug"] | false;

    JsonObject& groups = root["groups"];
    if (groups.success()) {
        for (auto kv : groups) {
            JsonArray& members = kv.value.as<JsonArray>();
            if (!members.success()) {
                error = F("group '");
                error += kv.key;
                error += F("' is not an array");
                return false;
            }

            std::vector<int> entities;
            for (auto member : members) {
                if (member.is<const char*>()) {
                    const String topic(member.as<const char*>());
                    if (!topic.length()) {
                        continue;
                    }
                    entities.push_back(findOrAddTopic(out, topic, String()));
                } else if (member.is<int>()) {
                    const int relay = member.as<int>();
#if RELAY_SUPPORT
                    if ((relay >= 0) && (static_cast<size_t>(relay) < relayCount())) {
                        entities.push_back(findOrAddRelay(out, relay));
                        continue;
                    }
#endif
                    error = F("no such relay in group '");
                    error += kv.key;
                    error += '\'';
                    return false;
                }
            }

            out.group_names.push_back(kv.key);
            out.groups.push_back(std::move(entities));
        }
    }

    JsonArray& pages = root["pages"];
    if (!pages.success()) {
        error = F("'pages' must be an array");
        return false;
    }

    size_t page_index = 0;
    for (auto page_variant : pages) {
        JsonObject& page_obj = page_variant.as<JsonObject>();
        if (!page_obj.success()) {
            error = F("page is not an object");
            return false;
        }

        const char* type = page_obj["type"].as<const char*>();
        if (type && strcmp(type, "grid")) {
            warnings += F("page type '");
            warnings += type;
            warnings += F("' shown as grid; ");
        }

        Page page;
        page.title = sanitize(page_obj["title"].as<const char*>(), build::HeadingMax);

        JsonArray& items = page_obj["items"];
        if (items.success()) {
            for (auto item_variant : items) {
                JsonObject& item_obj = item_variant.as<JsonObject>();
                if (!item_obj.success()) {
                    error = F("item is not an object");
                    return false;
                }

                Item item;
                item.name = sanitize(item_obj["name"].as<const char*>(), build::NameMax);

                const char* item_type = item_obj["type"].as<const char*>();
                uint16_t fallback = icons::LightSwitch;
                if (!item_type || !strcmp(item_type, "switch")) {
                    item.type = ItemType::Switch;
                } else if (!strcmp(item_type, "button")) {
                    item.type = ItemType::Button;
                    fallback = icons::GestureTapButton;
                } else if (!strcmp(item_type, "scene")) {
                    item.type = ItemType::Scene;
                    fallback = icons::LightbulbGroup;
                } else if (!strcmp(item_type, "text")) {
                    item.type = ItemType::Text;
                    fallback = 0;
                } else {
                    error = F("unknown item type '");
                    error += item_type;
                    error += '\'';
                    return false;
                }

                if (!parseTarget(item_obj, out, item.target, error)) {
                    error = String(F("page ")) + page_index + F(" item '") + item.name + F("': ") + error;
                    return false;
                }

                const bool need_group = (item.type == ItemType::Scene);
                if (need_group ? (item.target.group < 0) : (item.target.entity < 0)) {
                    error = String(F("page ")) + page_index + F(" item '") + item.name
                        + (need_group ? F("': scene needs \"group\"") : F("': needs \"topic\" or \"relay\""));
                    return false;
                }

                item.icon = resolveIcon(item_obj["icon"].as<const char*>(), fallback, warnings);

                out.items.push_back(std::move(item));
                page.items.push_back(out.items.size() - 1);
            }
        }

        out.pages.push_back(std::move(page));
        ++page_index;
    }

    JsonArray& buttons = root["buttons"];
    if (buttons.success()) {
        for (auto button_variant : buttons) {
            Target target;
            JsonObject& button_obj = button_variant.as<JsonObject>();
            if (button_obj.success() && !parseTarget(button_obj, out, target, error)) {
                error = String(F("button ")) + out.buttons.size() + F(": ") + error;
                return false;
            }
            out.buttons.push_back(target);
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Boot: crash-loop guard, safeboot (Tasmota factory partition), OTA layout
// -----------------------------------------------------------------------------

namespace boot {

constexpr uint32_t Magic { 0x4E535042 }; // NSPB

RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_count;

uint32_t count_at_boot { 0 };
bool cleared { false };

// Only crashes count, plus software restarts that ESPurna did not request itself
// (ESPurna always stores a custom reason before its own restarts)
bool counted(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_UNKNOWN:
        return true;
    case ESP_RST_SW:
        return customResetReason() == CustomResetReason::None;
    default:
        break;
    }

    return false;
}

const esp_partition_t* factory() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

// esp_ota_get_next_update_partition() falls back to the first OTA slot, which is the running
// one when the table has a single OTA app (Tasmota safeboot layout). Updater would then erase
// the running image
bool otaPossible() {
    const auto* running = esp_ota_get_running_partition();
    const auto* next = esp_ota_get_next_update_partition(nullptr);
    return next && running && (next->address != running->address);
}

bool switchToSafeboot(String& error) {
    const auto* part = factory();
    if (!part) {
        error = F("no factory (safeboot) app partition");
        return false;
    }

    const auto* running = esp_ota_get_running_partition();
    if (running && (running->address == part->address)) {
        error = F("already running from the factory partition");
        return false;
    }

    const auto result = esp_ota_set_boot_partition(part);
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        return false;
    }

    return true;
}

void setup() {
    const auto reason = esp_reset_reason();
    if ((rtc_magic != Magic) || (reason == ESP_RST_POWERON)) {
        rtc_magic = Magic;
        rtc_count = 0;
    }

    if (counted(reason)) {
        ++rtc_count;
    }

    count_at_boot = rtc_count;
    if (rtc_count >= build::CrashLoopMax) {
        rtc_count = 0;
        DEBUG_MSG_P(PSTR("[NSPANEL] %u unstable boots in a row, switching to safeboot\n"), count_at_boot);

        String error;
        if (switchToSafeboot(error)) {
            delay(100);
            esp_restart();
        }

        DEBUG_MSG_P(PSTR("[NSPANEL] Safeboot switch failed: %s\n"), error.c_str());
    }
}

void loop() {
    if (!cleared && (millis() >= build::CrashLoopUptimeMs)) {
        rtc_count = 0;
        cleared = true;
    }
}

} // namespace boot

// -----------------------------------------------------------------------------
// Filesystem
// -----------------------------------------------------------------------------

namespace storage {

bool mounted { false };
String label;

bool mount() {
    if (mounted) {
        return true;
    }

    const auto* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) {
        DEBUG_MSG_P(PSTR("[NSPANEL] No data/spiffs partition\n"));
        return false;
    }

    label = part->label;
    if (!LittleFS.begin(false, "/littlefs", 4, part->label)) {
        DEBUG_MSG_P(PSTR("[NSPANEL] LittleFS mount failed on '%s', formatting\n"), part->label);
        if (!LittleFS.begin(true, "/littlefs", 4, part->label)) {
            DEBUG_MSG_P(PSTR("[NSPANEL] LittleFS format failed\n"));
            return false;
        }
    }

    mounted = true;
    DEBUG_MSG_P(PSTR("[NSPANEL] LittleFS on '%s' (%u / %u bytes used)\n"),
        part->label, LittleFS.usedBytes(), LittleFS.totalBytes());

    return true;
}

bool read(String& out) {
    if (!mounted || !LittleFS.exists(build::ConfigPath)) {
        return false;
    }

    File file = LittleFS.open(build::ConfigPath, "r");
    if (!file) {
        return false;
    }

    const size_t size = file.size();
    if (!size || (size > build::ConfigMax)) {
        file.close();
        return false;
    }

    out = String();
    out.reserve(size);
    char buf[256];
    while (file.available()) {
        const auto len = file.readBytes(buf, sizeof(buf));
        if (!len) {
            break;
        }
        out.concat(buf, len);
    }
    file.close();

    return out.length() == size;
}

bool write(const char* data, size_t size) {
    if (!mounted) {
        return false;
    }

    File file = LittleFS.open(build::ConfigTmpPath, "w");
    if (!file) {
        return false;
    }

    const auto written = file.write(reinterpret_cast<const uint8_t*>(data), size);
    file.close();
    if (written != size) {
        LittleFS.remove(build::ConfigTmpPath);
        return false;
    }

    if (!LittleFS.rename(build::ConfigTmpPath, build::ConfigPath)) {
        LittleFS.remove(build::ConfigPath);
        return LittleFS.rename(build::ConfigTmpPath, build::ConfigPath);
    }

    return true;
}

} // namespace storage

// -----------------------------------------------------------------------------
// Display state & serial transport
// -----------------------------------------------------------------------------

enum class Screen {
    Unknown,
    Startup,
    Screensaver,
    Card,
};

const char* screenName(Screen screen) {
    switch (screen) {
    case Screen::Unknown:
        return "unknown";
    case Screen::Startup:
        return "startup";
    case Screen::Screensaver:
        return "screensaver";
    case Screen::Card:
        return "card";
    }

    return "";
}

struct Card {
    uint16_t page;
    uint16_t first;
    uint16_t count;
    String title;
};

struct Display {
    bool connected { false };
    int version { 0 };
    String model;
    Screen screen { Screen::Unknown };
    size_t card { 0 };
    std::vector<Card> cards;

    bool dirty { false };
    uint32_t dirty_since { 0 };

    uint32_t startup_request { 0 };
    int last_minute { -1 };
    int last_mday { -1 };
    uint8_t status_mask { 0xFF };
    String temperature_text;

    std::deque<String> queue;
    uint32_t last_tx { 0 };

    std::deque<String> events;
    uint32_t rx_frames { 0 };
    uint32_t tx_frames { 0 };
    uint32_t crc_errors { 0 };
    uint32_t dropped { 0 };
};

Display display;

namespace serial {

HardwareSerial& port() {
    return Serial2;
}

struct Parser {
    enum class State {
        Header1,
        Header2,
        Length1,
        Length2,
        Payload,
        Crc1,
        Crc2,
    };

    State state { State::Header1 };
    uint16_t length { 0 };
    uint16_t crc { 0xFFFF };
    uint16_t received_crc { 0 };
    uint32_t raw { 0 };
    String payload;

    void update(uint8_t byte) {
        crc = crc16(&byte, 1, crc);
    }

    // true when 'payload' holds a complete, CRC-checked frame
    bool feed(uint8_t byte, bool& raw_startup) {
        switch (state) {
        case State::Header1:
            // TFT power-up announcement 00 00 00 FF FF FF 88 FF FF FF
            raw = (raw << 8) | byte;
            if (raw == 0x88FFFFFFul) {
                raw_startup = true;
                raw = 0;
            }
            if (byte == 0x55) {
                crc = 0xFFFF;
                update(byte);
                state = State::Header2;
            }
            break;

        case State::Header2:
            if (byte == 0xBB) {
                update(byte);
                state = State::Length1;
            } else if (byte == 0x55) {
                crc = 0xFFFF;
                update(byte);
            } else {
                state = State::Header1;
            }
            break;

        case State::Length1:
            length = byte;
            update(byte);
            state = State::Length2;
            break;

        case State::Length2:
            length |= static_cast<uint16_t>(byte) << 8;
            update(byte);
            if (!length || (length > build::FrameMax)) {
                state = State::Header1;
                ++display.crc_errors;
                break;
            }
            payload = String();
            payload.reserve(length);
            state = State::Payload;
            break;

        case State::Payload:
            payload += static_cast<char>(byte);
            update(byte);
            if (payload.length() >= length) {
                state = State::Crc1;
            }
            break;

        case State::Crc1:
            received_crc = byte;
            state = State::Crc2;
            break;

        case State::Crc2:
            received_crc |= static_cast<uint16_t>(byte) << 8;
            state = State::Header1;
            if (received_crc == crc) {
                return true;
            }
            ++display.crc_errors;
            if (config.debug) {
                DEBUG_MSG_P(PSTR("[NSPANEL] CRC mismatch (%04X != %04X)\n"), received_crc, crc);
            }
            break;
        }

        return false;
    }
};

Parser parser;

void begin() {
    auto& serial = port();
    serial.setRxBufferSize(1024);
    serial.setTxBufferSize(1024);
    serial.begin(build::Baudrate, SERIAL_8N1, build::RxPin, build::TxPin);
}

void writeFrame(const String& payload) {
    const size_t len = payload.length();
    const uint8_t header[4] {
        0x55, 0xBB,
        static_cast<uint8_t>(len & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
    };

    uint16_t crc = crc16(header, sizeof(header));
    crc = crc16(reinterpret_cast<const uint8_t*>(payload.c_str()), len, crc);

    const uint8_t footer[2] {
        static_cast<uint8_t>(crc & 0xFF),
        static_cast<uint8_t>((crc >> 8) & 0xFF),
    };

    auto& serial = port();
    serial.write(header, sizeof(header));
    serial.write(reinterpret_cast<const uint8_t*>(payload.c_str()), len);
    serial.write(footer, sizeof(footer));

    ++display.tx_frames;
    if (config.debug) {
        DEBUG_MSG_P(PSTR("[NSPANEL] -> %s\n"), payload.c_str());
    }
}

// raw Nextion command, terminated by FF FF FF
void writeRaw(const char* command) {
    auto& serial = port();
    serial.write(reinterpret_cast<const uint8_t*>(command), strlen(command));
    const uint8_t end[3] {0xFF, 0xFF, 0xFF};
    serial.write(end, sizeof(end));
}

String commandName(const String& payload) {
    const auto pos = payload.indexOf('~');
    return (pos < 0) ? payload : payload.substring(0, pos);
}

// 'coalesce' replaces the latest queued frame of the same command, unless there is a
// page change queued after it (frames after pageType~ belong to the new page)
void enqueue(String payload, bool coalesce = false) {
    if (payload.length() > build::FrameMax) {
        DEBUG_MSG_P(PSTR("[NSPANEL] Frame too long (%u bytes), dropped\n"), payload.length());
        return;
    }

    if (coalesce) {
        const auto name = commandName(payload);
        for (auto it = display.queue.rbegin(); it != display.queue.rend(); ++it) {
            if ((*it).startsWith(F("pageType~"))) {
                break;
            }
            if (commandName(*it) == name) {
                *it = std::move(payload);
                return;
            }
        }
    }

    if (display.queue.size() >= build::QueueMax) {
        display.queue.pop_front();
        ++display.dropped;
    }

    display.queue.push_back(std::move(payload));
}

void txLoop() {
    if (display.queue.empty()) {
        return;
    }

    const auto now = millis();
    if (now - display.last_tx < build::TxGapMs) {
        return;
    }

    if (port().availableForWrite() < static_cast<int>(display.queue.front().length() + 6)) {
        return;
    }

    writeFrame(display.queue.front());
    display.queue.pop_front();
    display.last_tx = now;
}

} // namespace serial

// -----------------------------------------------------------------------------
// Entities
// -----------------------------------------------------------------------------

int entityState(const Entity& entity) {
#if RELAY_SUPPORT
    if (entity.relay >= 0) {
        switch (relayStatus(entity.relay)) {
        case RelayStatus::On:
            return 1;
        case RelayStatus::Off:
            return 0;
        default:
            return -1;
        }
    }
#endif
    return entity.state;
}

int entityState(int index) {
    if ((index < 0) || (static_cast<size_t>(index) >= config.entities.size())) {
        return -1;
    }

    return entityState(config.entities[index]);
}

// -1 when nothing is known, 1 when anything is on
int groupState(int group) {
    if ((group < 0) || (static_cast<size_t>(group) >= config.groups.size())) {
        return -1;
    }

    int out = -1;
    for (const auto entity : config.groups[group]) {
        const auto state = entityState(entity);
        if (state == 1) {
            return 1;
        }
        if (state == 0) {
            out = 0;
        }
    }

    return out;
}

void entitySet(int index, Action action) {
    if ((index < 0) || (static_cast<size_t>(index) >= config.entities.size())) {
        return;
    }

    const auto& entity = config.entities[index];
#if RELAY_SUPPORT
    if (entity.relay >= 0) {
        if (action == Action::Toggle) {
            relayToggle(entity.relay);
        } else {
            relayStatus(entity.relay, action == Action::On);
        }
        return;
    }
#endif

#if MQTT_SUPPORT
    const String* payload = &config.toggle;
    switch (action) {
    case Action::On:
        payload = &config.on;
        break;
    case Action::Off:
        payload = &config.off;
        break;
    case Action::Toggle:
        if (entity.state >= 0) {
            payload = entity.state ? &config.off : &config.on;
        }
        break;
    }

    if (entity.command.length()) {
        mqttSendRaw(entity.command.c_str(), payload->c_str(), false);
    }
#endif
}

void groupSet(int group, Action action) {
    if ((group < 0) || (static_cast<size_t>(group) >= config.groups.size())) {
        return;
    }

    if (action == Action::Toggle) {
        action = (groupState(group) == 1) ? Action::Off : Action::On;
    }

    for (const auto entity : config.groups[group]) {
        entitySet(entity, action);
    }
}

bool cardContains(size_t card, int entity) {
    if (card >= display.cards.size()) {
        return false;
    }

    const auto& info = display.cards[card];
    const auto& page = config.pages[info.page];
    for (size_t n = info.first; n < (info.first + info.count); ++n) {
        const auto& item = config.items[page.items[n]];
        if (item.target.entity == entity) {
            return true;
        }

        if (item.target.group >= 0) {
            for (const auto member : config.groups[item.target.group]) {
                if (member == entity) {
                    return true;
                }
            }
        }
    }

    return false;
}

void entityChanged(int entity) {
    if ((display.screen == Screen::Card) && cardContains(display.card, entity)) {
        if (!display.dirty) {
            display.dirty = true;
            display.dirty_since = millis();
        }
    }
}

// -----------------------------------------------------------------------------
// Weather (Open-Meteo, fetched in a separate task)
// -----------------------------------------------------------------------------

namespace weather {

struct Day {
    int code { -1 };
    float tmax { 0.0f };
    float tmin { 0.0f };
    int wday { 0 };
};

struct Data {
    bool valid { false };
    float temperature { 0.0f };
    int code { -1 };
    bool day { true };
    Day days[4];
    size_t count { 0 };
    time_t updated { 0 };
};

enum State : int {
    Idle,
    Running,
    Done,
    Failed,
};

std::atomic<int> state { Idle };
Data result;
Data current;
char url[256];
char last_error[64] { "never" };
uint32_t next_ms { 15000 };
uint32_t failures { 0 };

void task(void*) {
    Data out;
    int code = -1;
    String body;

    {
        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(10);

        HTTPClient http;
        http.setConnectTimeout(5000);
        http.setTimeout(5000);
        http.setReuse(false);
        if (http.begin(client, url)) {
            code = http.GET();
            if (code == 200) {
                body = http.getString();
            }
            http.end();
        }
    }

    if (code != 200) {
        snprintf(last_error, sizeof(last_error), "HTTP %d", code);
        state.store(Failed);
        vTaskDelete(nullptr);
        return;
    }

    {
        DynamicJsonBuffer buffer(1024);
        JsonObject& root = buffer.parseObject(body.c_str());
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
                const size_t count = std::min<size_t>(4, times.size());
                for (size_t n = 0; n < count; ++n) {
                    int y = 0, m = 0, d = 0;
                    const char* date = times[n].as<const char*>();
                    if (!date || (sscanf(date, "%d-%d-%d", &y, &m, &d) != 3)) {
                        break;
                    }
                    out.days[n].wday = weekday(y, m, d);
                    out.days[n].code = codes[n] | -1;
                    out.days[n].tmax = tmax[n].as<float>();
                    out.days[n].tmin = tmin[n].as<float>();
                    out.count = n + 1;
                }
            }
        }
    }

    if (!out.valid) {
        snprintf(last_error, sizeof(last_error), "invalid response (%u bytes)", body.length());
        state.store(Failed);
        vTaskDelete(nullptr);
        return;
    }

    out.updated = ::time(nullptr);
    result = out;
    snprintf(last_error, sizeof(last_error), "ok");
    state.store(Done);
    vTaskDelete(nullptr);
}

void request() {
    next_ms = millis();
}

void sendUpdate();

void loop() {
    if (!config.weather) {
        return;
    }

    const auto now = millis();
    switch (state.load()) {
    case Running:
        break;

    case Done:
        current = result;
        state.store(Idle);
        failures = 0;
        next_ms = now + config.refresh * 1000ul;
        DEBUG_MSG_P(PSTR("[NSPANEL] Weather: %.1fC code %d, %u days\n"),
            current.temperature, current.code, current.count);
        if (display.screen == Screen::Screensaver) {
            sendUpdate();
        }
        break;

    case Failed:
        state.store(Idle);
        ++failures;
        next_ms = now + build::WeatherRetryMs;
        DEBUG_MSG_P(PSTR("[NSPANEL] Weather update failed: %s\n"), last_error);
        break;

    case Idle:
        if (static_cast<int32_t>(now - next_ms) < 0) {
            break;
        }

        if (!wifiConnected()) {
            next_ms = now + 5000;
            break;
        }

        if (ESP.getFreeHeap() < build::WeatherHeapMin) {
            snprintf(last_error, sizeof(last_error), "low heap (%u)", ESP.getFreeHeap());
            next_ms = now + build::WeatherRetryMs;
            break;
        }

        snprintf(url, sizeof(url),
            "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
            "&current=temperature_2m,weather_code,is_day"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min"
            "&timezone=auto&forecast_days=4",
            config.lat, config.lon);

        state.store(Running);
        if (pdPASS != xTaskCreate(task, "nspweather", build::WeatherStack, nullptr, 1, nullptr)) {
            state.store(Idle);
            snprintf(last_error, sizeof(last_error), "task create failed");
            next_ms = now + build::WeatherRetryMs;
        }
        break;
    }
}

} // namespace weather

// -----------------------------------------------------------------------------
// NTC temperature (GPIO38, ADC1)
// -----------------------------------------------------------------------------

namespace ntc {

float value { NAN };
float millivolts { 0.0f };
uint32_t last_read { 0 };
uint32_t last_publish { 0 };
bool first { true };

void read() {
    uint32_t sum = 0;
    for (int n = 0; n < 8; ++n) {
        sum += analogReadMilliVolts(build::NtcPin);
    }
    millivolts = sum / 8.0f;

    if ((millivolts < 10.0f) || (millivolts > (build::NtcSupplyMv - 10.0f))) {
        value = NAN;
        return;
    }

    const float resistance = build::NtcPullup * millivolts / (build::NtcSupplyMv - millivolts);
    const float kelvin = 1.0f / ((1.0f / build::NtcT0) + (logf(resistance / build::NtcR0) / build::NtcBeta));
    const float celsius = kelvin - 273.15f + config.temperature_offset;

    value = std::isnan(value) ? celsius : (value * 0.7f + celsius * 0.3f);
}

String text() {
    if (!config.temperature || std::isnan(value)) {
        return String();
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0" "C", value);
    return buf;
}

void loop() {
    if (!config.temperature) {
        return;
    }

    const auto now = millis();
    if (first || (now - last_read >= build::NtcReadMs)) {
        first = false;
        last_read = now;
        read();

        if (config.temperature_show && (display.screen == Screen::Screensaver)) {
            const auto current = text();
            if (current != display.temperature_text) {
                weather::sendUpdate();
            }
        }
    }

#if MQTT_SUPPORT
    if (config.temperature_publish && !std::isnan(value)
        && (now - last_publish >= config.temperature_publish * 1000ul)
        && mqttConnected())
    {
        last_publish = now;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", value);
        mqttSend("temperature", buf);
    }
#endif
}

} // namespace ntc

// -----------------------------------------------------------------------------
// Buzzer
// -----------------------------------------------------------------------------

namespace buzzer {

bool active { false };
uint32_t until { 0 };

void setup() {
    ledcSetup(build::BuzzerChannel, 2700, 8);
    ledcAttachPin(build::BuzzerPin, build::BuzzerChannel);
    ledcWrite(build::BuzzerChannel, 0);
}

void beep() {
    if (!config.beep) {
        return;
    }

    ledcWrite(build::BuzzerChannel, 128);
    active = true;
    until = millis() + 40;
}

void loop() {
    if (active && (static_cast<int32_t>(millis() - until) >= 0)) {
        ledcWrite(build::BuzzerChannel, 0);
        active = false;
    }
}

} // namespace buzzer

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

String forecastValue(const weather::Day& day) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d\xC2\xB0/%d\xC2\xB0",
        static_cast<int>(lroundf(day.tmax)), static_cast<int>(lroundf(day.tmin)));
    if (strlen(buf) <= build::ForecastMax) {
        return buf;
    }

    snprintf(buf, sizeof(buf), "%d\xC2\xB0", static_cast<int>(lroundf(day.tmax)));
    return buf;
}

void weatherGroup(String& out, uint16_t icon, uint16_t color, const String& name, const String& value) {
    out += F("~~~");
    out += utf8(icon);
    out += '~';
    if (icon) {
        out += color;
    }
    out += '~';
    out += name;
    out += '~';
    out += value;
}

void weather::sendUpdate() {
    String out(F("weatherUpdate"));
    out.reserve(200);

    const auto& data = weather::current;
    if (config.weather && data.valid) {
        const auto main = icons::weather(data.code, data.day);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\xC2\xB0" "C", static_cast<int>(lroundf(data.temperature)));
        weatherGroup(out, main.icon, main.color, String(), buf);

        for (size_t n = 0; n < 4; ++n) {
            if (n < data.count) {
                const auto& day = data.days[n];
                const auto icon = icons::weather(day.code, true);
                weatherGroup(out, icon.icon, icon.color, ru::WeekdayShort[day.wday % 7], forecastValue(day));
            } else {
                weatherGroup(out, 0, 0, String(), String());
            }
        }
    } else {
        for (size_t n = 0; n < 5; ++n) {
            weatherGroup(out, 0, 0, String(), String());
        }
    }

    // 6th group switches the screensaver to the alternative layout (second main value)
    display.temperature_text = config.temperature_show ? ntc::text() : String();
    if (display.temperature_text.length()) {
        weatherGroup(out, icons::Thermometer, icons::colors::White, String(), display.temperature_text);
    } else {
        out += F("~~~~~~");
    }

    serial::enqueue(std::move(out), true);
}

uint8_t statusMask() {
    uint8_t out = 0;
    if (!wifiConnected()) {
        out |= 1;
    }
#if MQTT_SUPPORT
    if (!mqttConnected()) {
        out |= 2;
    }
#endif
    return out;
}

void sendStatus() {
    display.status_mask = statusMask();

    String out(F("statusUpdate~"));
    if (display.status_mask & 1) {
        out += utf8(icons::WifiOff);
        out += '~';
        out += icons::colors::Red;
    } else if (display.status_mask & 2) {
        out += utf8(icons::AlertCircleOutline);
        out += '~';
        out += icons::colors::Red;
    } else {
        out += '~';
    }
    out += F("~~~~");

    serial::enqueue(std::move(out), true);
}

void sendDimmode() {
    String out(F("dimmode~"));
    out += config.dim;
    out += '~';
    out += config.brightness;
    out += '~';
    out += icons::colors::Background;
    serial::enqueue(std::move(out), true);
}

void sendTimeout() {
    String out(F("timeout~"));
    out += config.timeout;
    serial::enqueue(std::move(out), true);
}

void sendClock(bool force) {
    const time_t now = ::time(nullptr);
    if (now < 1609459200) { // 2021-01-01, not synced yet
        return;
    }

    tm t;
    localtime_r(&now, &t);

    if (force || (t.tm_min != display.last_minute)) {
        display.last_minute = t.tm_min;
        char buf[24];
        snprintf(buf, sizeof(buf), "time~%02d:%02d~", t.tm_hour, t.tm_min);
        serial::enqueue(buf, true);
    }

    if (force || (t.tm_mday != display.last_mday)) {
        display.last_mday = t.tm_mday;
        serial::enqueue(String(F("date~")) + ru::date(t), true);
    }
}

void buildCards() {
    const size_t capacity = (display.version >= 51) ? 8 : 6;
    display.cards.clear();

    for (size_t page = 0; page < config.pages.size(); ++page) {
        const auto& info = config.pages[page];
        const size_t total = info.items.size();
        const size_t chunks = std::max<size_t>(1, (total + capacity - 1) / capacity);

        for (size_t chunk = 0; chunk < chunks; ++chunk) {
            Card card;
            card.page = page;
            card.first = chunk * capacity;
            card.count = std::min(capacity, total - std::min(total, chunk * capacity));
            card.title = info.title;
            if (chunks > 1) {
                card.title += ' ';
                card.title += String(chunk + 1);
                card.title += '/';
                card.title += String(chunks);
            }
            utf8Truncate(card.title, build::HeadingMax);
            display.cards.push_back(std::move(card));
        }
    }

    if (display.card >= display.cards.size()) {
        display.card = 0;
    }
}

const char* cardType(const Card& card) {
    return ((display.version >= 51) && (card.count > 6))
        ? "cardGrid2"
        : "cardGrid";
}

uint16_t stateColor(int state) {
    return (state == 1) ? config.color_on
        : (state == 0) ? config.color_off
        : config.color_unknown;
}

void renderItem(String& out, uint16_t index) {
    const auto& item = config.items[index];

    const char* type = "switch";
    String icon = item.icon;
    uint16_t color = config.color_unknown;
    String value;

    switch (item.type) {
    case ItemType::Switch: {
        const auto state = entityState(item.target.entity);
        color = stateColor(state);
        value = (state == 1) ? F("1") : F("0");
        break;
    }

    case ItemType::Button:
        type = "button";
        color = stateColor(entityState(item.target.entity));
        break;

    case ItemType::Scene:
        type = "button";
        color = stateColor(groupState(item.target.group));
        break;

    case ItemType::Text: {
        type = "text";
        color = icons::colors::White;
        if ((item.target.entity >= 0) && !icon.length()) {
            icon = utf8Prefix(sanitize(config.entities[item.target.entity].value.c_str(), 32), 4);
        }
        if (item.target.entity >= 0) {
            value = sanitize(config.entities[item.target.entity].value.c_str(), 32);
        }
        break;
    }
    }

    out += '~';
    out += type;
    out += F("~i");
    out += index;
    out += '~';
    out += icon;
    out += '~';
    out += color;
    out += '~';
    out += item.name;
    out += '~';
    out += value;
}

void renderNavHidden(String& out) {
    out += F("~delete~~~~~");
}

void renderNav(String& out, const char* id, uint16_t icon, const String& name) {
    String label = name;
    utf8Truncate(label, build::NameMax);

    out += F("~button~");
    out += id;
    out += '~';
    out += utf8(icon);
    out += '~';
    out += icons::colors::White;
    out += '~';
    out += label;
    out += '~';
}

void showScreensaver();

void showCard(size_t index, bool full) {
    if (display.cards.empty()) {
        buildCards();
    }

    if (display.cards.empty()) {
        showScreensaver();
        return;
    }

    if (index >= display.cards.size()) {
        index = 0;
    }

    display.card = index;
    display.screen = Screen::Card;
    display.dirty = false;

    const auto& card = display.cards[index];
    const size_t total = display.cards.size();

    if (full) {
        serial::enqueue(String(F("pageType~")) + cardType(card));
        sendTimeout();
    }

    String out(F("entityUpd~"));
    out.reserve(640);
    out += card.title;

    // Linear navigation: the screensaver sits left of the first card, the last card has no right arrow
    if (index == 0) {
        renderNav(out, "navHome", icons::Home, String());
    } else {
        renderNav(out, "navPrev", icons::ArrowLeftBold, display.cards[index - 1].title);
    }

    if ((index + 1) < total) {
        renderNav(out, "navNext", icons::ArrowRightBold, display.cards[index + 1].title);
    } else {
        renderNavHidden(out);
    }

    const auto& page = config.pages[card.page];
    for (size_t n = card.first; n < (card.first + card.count); ++n) {
        renderItem(out, page.items[n]);
    }

    if (out.length() > 700) {
        DEBUG_MSG_P(PSTR("[NSPANEL] entityUpd is %u bytes, TFT may truncate it (700)\n"), out.length());
    }

    serial::enqueue(std::move(out), true);
}

void showScreensaver() {
    display.screen = Screen::Screensaver;
    display.dirty = false;
    serial::enqueue(F("pageType~screensaver"));
    sendTimeout();
    weather::sendUpdate();
    sendStatus();
}

void requestStartup() {
    display.startup_request = millis();
    serial::enqueue(F("pageType~pageStartup"));
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

void onStartup(int version, const String& model) {
    display.connected = true;
    display.version = version;
    display.model = model;
    display.screen = Screen::Startup;

    DEBUG_MSG_P(PSTR("[NSPANEL] TFT startup: version %d, model '%s'\n"), version, model.c_str());
    if ((version < 50) || (model != F("eu"))) {
        DEBUG_MSG_P(PSTR("[NSPANEL] Unexpected TFT version / model, expected >= 50 and 'eu'\n"));
    }

    buildCards();
    sendClock(true);
    sendDimmode();
    showScreensaver();
}

void itemTap(size_t index, bool has_value, bool value) {
    if (index >= config.items.size()) {
        return;
    }

    const auto& item = config.items[index];
    buzzer::beep();

    switch (item.type) {
    case ItemType::Switch:
        entitySet(item.target.entity,
            has_value ? (value ? Action::On : Action::Off) : Action::Toggle);
        break;

    case ItemType::Button:
        entitySet(item.target.entity, Action::On);
        break;

    case ItemType::Scene:
        groupSet(item.target.group, item.target.action);
        break;

    case ItemType::Text:
        break;
    }
}

void recordEvent(const String& message) {
    if (display.events.size() >= build::EventsMax) {
        display.events.pop_front();
    }
    display.events.push_back(message);
}

void onMessage(const String& message) {
    ++display.rx_frames;
    recordEvent(message);

    if (config.debug) {
        DEBUG_MSG_P(PSTR("[NSPANEL] <- %s\n"), message.c_str());
    }

#if MQTT_SUPPORT
    if (mqttConnected()) {
        mqttSendRaw(mqttTopic("nspanel").c_str(), message.c_str(), false);
    }
#endif

    String parts[6];
    size_t count = 0;
    int start = 0;
    while ((count < 6) && (start <= static_cast<int>(message.length()))) {
        int pos = message.indexOf(',', start);
        if ((pos < 0) || (count == 5)) {
            pos = message.length();
        }
        parts[count++] = message.substring(start, pos);
        start = pos + 1;
    }

    if ((count < 2) || (parts[0] != F("event"))) {
        return;
    }

    const auto& type = parts[1];
    if (type == F("startup")) {
        onStartup(parts[2].toInt(), parts[3]);
        return;
    }

    if (type == F("sleepReached")) {
        showScreensaver();
        return;
    }

    if (type == F("renderCurrentPage")) {
        if (display.screen == Screen::Card) {
            showCard(display.card, false);
        }
        return;
    }

    if (type != F("buttonPress2") || (count < 4)) {
        return;
    }

    const auto& id = parts[2];
    const auto& action = parts[3];

    if (id == F("screensaver")) {
        if ((action == F("bExit")) || action.startsWith(F("swipe"))) {
            showCard(0, true);
        }
        return;
    }

    if ((action == F("bExit")) || (action == F("notifyAction"))) {
        showCard(display.card, true);
        return;
    }

    if (id == F("navHome")) {
        buzzer::beep();
        showScreensaver();
        return;
    }

    if (id == F("navPrev") || id == F("navNext")) {
        const size_t total = display.cards.size();
        if (id == F("navNext")) {
            if ((display.card + 1) < total) {
                buzzer::beep();
                showCard(display.card + 1, true);
            }
        } else if (display.card > 0) {
            buzzer::beep();
            showCard(display.card - 1, true);
        } else {
            buzzer::beep();
            showScreensaver();
        }
        return;
    }

    if ((id.length() > 1) && (id[0] == 'i')) {
        const auto index = id.substring(1).toInt();
        if (action == F("button")) {
            itemTap(index, false, false);
        } else if ((action == F("OnOff")) && (count >= 5)) {
            itemTap(index, true, parts[4].toInt() != 0);
        }
    }
}

void rxLoop() {
    auto& port = serial::port();
    size_t budget = 512;
    while (budget-- && port.available()) {
        const int byte = port.read();
        if (byte < 0) {
            break;
        }

        bool raw_startup = false;
        if (serial::parser.feed(static_cast<uint8_t>(byte), raw_startup)) {
            const String message = serial::parser.payload;
            serial::parser.payload = String();
            onMessage(message);
        }

        if (raw_startup) {
            DEBUG_MSG_P(PSTR("[NSPANEL] TFT power-up, sending recmod=1\n"));
            serial::writeRaw("recmod=1");
            display.connected = false;
            display.screen = Screen::Unknown;
        }
    }
}

// -----------------------------------------------------------------------------
// Config load / reload
// -----------------------------------------------------------------------------

PROGMEM_STRING(DefaultConfig,
    "{\"pages\":[{\"title\":\"NSPanel\",\"items\":["
    "{\"name\":\"Relay 1\",\"icon\":\"light-switch\",\"relay\":0},"
    "{\"name\":\"Relay 2\",\"icon\":\"light-switch\",\"relay\":1}]}],"
    "\"buttons\":[{\"relay\":0},{\"relay\":1}]}");

std::vector<String> subscribed;

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
#if MQTT_SUPPORT
    subscribed.clear();
    if (!mqttConnected()) {
        return;
    }

    for (const auto& entity : config.entities) {
        if (entity.topic.length()) {
            mqttSubscribeRaw(entity.topic.c_str());
            subscribed.push_back(entity.topic);
        }
    }
#endif
}

String last_load_error;
String last_load_warnings;

void load() {
    String text;
    const bool from_file = storage::read(text);
    if (!from_file) {
        text = DefaultConfig;
    }

    Config next;
    String error;
    String warnings;
    if (!parse(text.c_str(), next, error, warnings)) {
        last_load_error = error;
        DEBUG_MSG_P(PSTR("[NSPANEL] Config error: %s\n"), error.c_str());
        if (!config.pages.empty()) {
            return;
        }

        next = Config();
        warnings = String();
        error = String();
        parse(DefaultConfig, next, error, warnings);
    } else {
        last_load_error = String();
    }

    last_load_warnings = warnings;
    if (warnings.length()) {
        DEBUG_MSG_P(PSTR("[NSPANEL] Config warnings: %s\n"), warnings.c_str());
    }

    // keep known MQTT states across reloads
    for (auto& entity : next.entities) {
        for (const auto& old : config.entities) {
            if (entity.topic.length() && (entity.topic == old.topic)) {
                entity.state = old.state;
                entity.value = old.value;
                break;
            }
        }
    }

    const bool weather_changed = (next.weather != config.weather)
        || (next.lat != config.lat) || (next.lon != config.lon);

    unsubscribeAll();
    config = std::move(next);
    subscribeAll();

    DEBUG_MSG_P(PSTR("[NSPANEL] Config %s: %u pages, %u items, %u entities, %u groups\n"),
        from_file ? build::ConfigPath : "(default)",
        config.pages.size(), config.items.size(), config.entities.size(), config.groups.size());

    if (weather_changed) {
        weather::current = weather::Data();
        weather::request();
    }

    buildCards();
    if (display.connected) {
        sendDimmode();
        if (display.screen == Screen::Card) {
            showCard(display.card, true);
        } else {
            showScreensaver();
        }
    }
}

void reloadOnce() {
    load();
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

#if MQTT_SUPPORT
void mqttCallback(unsigned int type, StringView topic, StringView payload) {
    switch (type) {
    case MQTT_CONNECT_EVENT:
        subscribeAll();
        break;

    case MQTT_DISCONNECT_EVENT:
        subscribed.clear();
        break;

    case MQTT_MESSAGE_EVENT:
        for (size_t index = 0; index < config.entities.size(); ++index) {
            auto& entity = config.entities[index];
            if (!entity.topic.length() || !topic.equals(StringView(entity.topic))) {
                continue;
            }

            const int state = payload.equals(StringView(config.on)) ? 1
                : payload.equals(StringView(config.off)) ? 0
                : -1;

            String value = payload.toString();
            utf8Truncate(value, 32);

            if ((state != entity.state) || (value != entity.value)) {
                entity.state = state;
                entity.value = std::move(value);
                entityChanged(index);
            }
        }
        break;
    }
}
#endif

#if RELAY_SUPPORT
void relayCallback(size_t id, bool) {
    for (size_t index = 0; index < config.entities.size(); ++index) {
        if (config.entities[index].relay == static_cast<int>(id)) {
            entityChanged(index);
        }
    }
}
#endif

#if BUTTON_SUPPORT
void buttonCallback(size_t id, ButtonEvent) {
    if (id >= config.buttons.size()) {
        return;
    }

    const auto& target = config.buttons[id];
    if (!target.valid) {
        return;
    }

    if (target.group >= 0) {
        groupSet(target.group, target.action);
    } else if (target.entity >= 0) {
        entitySet(target.entity, target.action);
    }

    if ((target.page >= 0) && display.connected) {
        for (size_t card = 0; card < display.cards.size(); ++card) {
            if (display.cards[card].page == target.page) {
                showCard(card, true);
                break;
            }
        }
    }
}
#endif

// -----------------------------------------------------------------------------
// Web
// -----------------------------------------------------------------------------

#if WEB_SUPPORT

void requestAuth(AsyncWebServerRequest* request) {
    request->requestAuthentication(systemHostname().c_str(), true);
}

void onEditor(AsyncWebServerRequest* request) {
    espurna::system::AsyncGuard guard;
    if (!guard.locked()) {
        webSendBusy(request);
        return;
    }

    if (!webAuthenticate(request)) {
        requestAuth(request);
        return;
    }

    request->send_P(200, "text/html; charset=utf-8", web::EditorPage);
}

void onGetConfig(AsyncWebServerRequest* request) {
    espurna::system::AsyncGuard guard;
    if (!guard.locked()) {
        webSendBusy(request);
        return;
    }

    if (!webAuthenticate(request)) {
        requestAuth(request);
        return;
    }

    String text;
    if (!storage::read(text)) {
        text = DefaultConfig;
    }

    request->send(200, "application/json; charset=utf-8", text);
}

struct Upload {
    AsyncWebServerRequest* request { nullptr };
    std::unique_ptr<char[]> buffer;
    size_t size { 0 };
    int code { 0 };
    String message;
};

Upload upload;

void uploadReset() {
    upload.request = nullptr;
    upload.buffer.reset(nullptr);
    upload.size = 0;
    upload.code = 0;
    upload.message = String();
}

void onPostBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    espurna::system::AsyncGuard guard;

    if (index == 0) {
        uploadReset();
        upload.request = request;
    }

    if ((upload.request != request) || upload.code) {
        return;
    }

    if (!guard.locked()) {
        upload.code = 503;
        upload.buffer.reset(nullptr);
        return;
    }

    if (!webAuthenticate(request)) {
        upload.code = 401;
        upload.buffer.reset(nullptr);
        return;
    }

    if (!total || (total > build::ConfigMax)) {
        upload.code = 413;
        upload.message = F("config too large");
        return;
    }

    if (index == 0) {
        upload.buffer.reset(new (std::nothrow) char[total + 1]);
        upload.size = total;
        if (!upload.buffer) {
            upload.code = 500;
            upload.message = F("out of memory");
            return;
        }
    }

    if (!upload.buffer || ((index + len) > upload.size)) {
        upload.code = 400;
        upload.message = F("unexpected body");
        return;
    }

    memcpy(upload.buffer.get() + index, data, len);
    if ((index + len) < total) {
        return;
    }

    upload.buffer[total] = '\0';

    Config check;
    String error;
    String warnings;
    if (!parse(upload.buffer.get(), check, error, warnings)) {
        upload.code = 400;
        upload.message = String(F("ERROR: ")) + error;
        upload.buffer.reset(nullptr);
        return;
    }

    if (!storage::write(upload.buffer.get(), total)) {
        upload.code = 500;
        upload.message = F("ERROR: write failed");
        upload.buffer.reset(nullptr);
        return;
    }

    upload.buffer.reset(nullptr);
    upload.code = 200;
    upload.message = String(F("OK")) + (warnings.length() ? (String(F(", warnings: ")) + warnings) : String());
    espurnaRegisterOnceUnique(reloadOnce);
}

void onPostConfig(AsyncWebServerRequest* request) {
    espurna::system::AsyncGuard guard;

    if ((upload.request != request) || !upload.code) {
        uploadReset();
        if (!guard.locked()) {
            webSendBusy(request);
            return;
        }
        if (!webAuthenticate(request)) {
            requestAuth(request);
            return;
        }
        request->send(400, "text/plain", F("ERROR: empty body"));
        return;
    }

    const auto code = upload.code;
    const auto message = upload.message;
    uploadReset();

    switch (code) {
    case 401:
        requestAuth(request);
        break;
    case 503:
        webSendBusy(request);
        break;
    default:
        request->send(code, "text/plain; charset=utf-8", message);
        break;
    }
}

PROGMEM_STRING(NoOtaMessage,
    "ERROR: this flash layout (Tasmota safeboot) has no second OTA slot. "
    "Run the terminal command `safeboot` and upload the image in the Tasmota safeboot web UI\n");

// no shared state, answered without the loop lock
void onNoOta(AsyncWebServerRequest* request) {
    request->send(400, "text/plain", NoOtaMessage);
}

void onNoOtaUpload(AsyncWebServerRequest*, String, size_t, uint8_t*, size_t, bool) {
}

void setupWeb() {
    auto& server = webServer();
    server.on("/nspanel", HTTP_GET, onEditor);
    server.on("/nspanel.json", HTTP_GET, onGetConfig);
    server.on("/nspanel.json", HTTP_POST | HTTP_PUT, onPostConfig, nullptr, onPostBody);

    if (!boot::otaPossible()) {
        server.rewrite("/upgrade", "/nspanel-noota");
        server.on("/nspanel-noota", HTTP_ANY, onNoOta, onNoOtaUpload);
        DEBUG_MSG_P(PSTR("[NSPANEL] No OTA slot, /upgrade is disabled (use `safeboot`)\n"));
    }
}

#endif // WEB_SUPPORT

// -----------------------------------------------------------------------------
// Terminal
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT

void terminalSafeboot(espurna::terminal::CommandContext&& ctx) {
    String error;
    if (!boot::switchToSafeboot(error)) {
        terminalError(ctx, error);
        return;
    }

    ctx.output.print(F("Next boot: factory (safeboot) partition, restarting\n"));
    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}

void terminalReload(espurna::terminal::CommandContext&& ctx) {
    load();
    if (last_load_error.length()) {
        terminalError(ctx, last_load_error);
        return;
    }
    terminalOK(ctx);
}

void terminalSend(espurna::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() < 2) {
        terminalError(ctx, F("nspanel.send <payload>"));
        return;
    }

    String payload;
    for (size_t n = 1; n < ctx.argv.size(); ++n) {
        if (n > 1) {
            payload += ' ';
        }
        payload += ctx.argv[n];
    }

    serial::enqueue(std::move(payload));
    terminalOK(ctx);
}

void terminalInfo(espurna::terminal::CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("TFT: %s, version %d, model '%s'\n"),
        display.connected ? "connected" : "not connected",
        display.version, display.model.c_str());
    ctx.output.printf_P(PSTR("Screen: %s, card %u / %u\n"),
        screenName(display.screen), display.card + 1, display.cards.size());
    ctx.output.printf_P(PSTR("Frames: rx %u, tx %u, crc errors %u, dropped %u, queued %u\n"),
        display.rx_frames, display.tx_frames, display.crc_errors, display.dropped, display.queue.size());
    ctx.output.printf_P(PSTR("Config: %u pages, %u items, %u entities, %u groups, %u subscribed\n"),
        config.pages.size(), config.items.size(), config.entities.size(), config.groups.size(), subscribed.size());
    if (last_load_error.length()) {
        ctx.output.printf_P(PSTR("Config error: %s\n"), last_load_error.c_str());
    }
    if (last_load_warnings.length()) {
        ctx.output.printf_P(PSTR("Config warnings: %s\n"), last_load_warnings.c_str());
    }
    if (storage::mounted) {
        ctx.output.printf_P(PSTR("FS: LittleFS on '%s', %u / %u bytes used\n"),
            storage::label.c_str(), LittleFS.usedBytes(), LittleFS.totalBytes());
    } else {
        ctx.output.print(F("FS: not mounted\n"));
    }
    ctx.output.printf_P(PSTR("Weather: %s, last: %s, %.1fC code %d, %u days\n"),
        config.weather ? "enabled" : "disabled", weather::last_error,
        weather::current.temperature, weather::current.code, weather::current.count);
    ctx.output.printf_P(PSTR("NTC: %.2f C (%.0f mV)\n"), ntc::value, ntc::millivolts);

    const auto* running = esp_ota_get_running_partition();
    ctx.output.printf_P(PSTR("Boot: running '%s', OTA slot %s, factory %s, unstable boots %u\n"),
        running ? running->label : "?",
        boot::otaPossible() ? "available" : "missing",
        boot::factory() ? "present" : "missing",
        boot::count_at_boot);

    ctx.output.print(F("Last events:\n"));
    for (const auto& event : display.events) {
        ctx.output.printf_P(PSTR("  %s\n"), event.c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Safeboot, "SAFEBOOT");
PROGMEM_STRING(PanelReload, "NSPANEL.RELOAD");
PROGMEM_STRING(PanelSend, "NSPANEL.SEND");
PROGMEM_STRING(PanelInfo, "NSPANEL.INFO");

static constexpr espurna::terminal::Command BootCommands[] PROGMEM {
    {Safeboot, terminalSafeboot},
};

static constexpr espurna::terminal::Command PanelCommands[] PROGMEM {
    {PanelReload, terminalReload},
    {PanelSend, terminalSend},
    {PanelInfo, terminalInfo},
};

#endif // TERMINAL_SUPPORT

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void bootLoop() {
    boot::loop();
}

void panelLoop() {
    rxLoop();
    serial::txLoop();

    const auto now = millis();
    if (!display.connected && (now - display.startup_request >= build::StartupRetryMs)) {
        requestStartup();
    }

    if (display.connected) {
        static uint32_t last_check = 0;
        if (now - last_check >= 1000) {
            last_check = now;
            sendClock(false);
            if ((display.screen == Screen::Screensaver) && (statusMask() != display.status_mask)) {
                sendStatus();
            }
        }

        if (display.dirty && (now - display.dirty_since >= build::RenderDelayMs)) {
            if (display.screen == Screen::Card) {
                showCard(display.card, false);
            }
            display.dirty = false;
        }
    }

    weather::loop();
    ntc::loop();
    buzzer::loop();
}

} // namespace
} // namespace nspanel
} // namespace espurna

void nspanelBootSetup() {
    using namespace espurna::nspanel;

    pinMode(build::DisplayPowerPin, OUTPUT);
    digitalWrite(build::DisplayPowerPin, LOW);
    gpioLock(build::DisplayPowerPin);

    pinMode(build::BuzzerPin, OUTPUT);
    digitalWrite(build::BuzzerPin, LOW);
    gpioLock(build::BuzzerPin);

    boot::setup();
    espurnaRegisterLoop(bootLoop);

#if TERMINAL_SUPPORT
    espurna::terminal::add(BootCommands);
#endif
}

void nspanelSetup() {
    using namespace espurna::nspanel;

    gpioLock(build::TxPin);
    gpioLock(build::RxPin);
    gpioLock(build::NtcPin);

    analogSetPinAttenuation(build::NtcPin, ADC_11db);
    buzzer::setup();
    serial::begin();

    storage::mount();
    load();

#if MQTT_SUPPORT
    mqttRegister(mqttCallback);
#endif
#if RELAY_SUPPORT
    relayOnStatusChange(relayCallback);
#endif
#if BUTTON_SUPPORT
    buttonSetCustomAction(buttonCallback);
#endif
#if WEB_SUPPORT
    setupWeb();
#endif
#if TERMINAL_SUPPORT
    espurna::terminal::add(PanelCommands);
#endif

    requestStartup();
    espurnaRegisterLoop(panelLoop);
}

#endif // NSPANEL_SUPPORT
