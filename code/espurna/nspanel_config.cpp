/*

NSPANEL MODULE - config parser

/nspanel.json -> Config (nspanel_config.h). Built for the firmware (NSPANEL_SUPPORT) and as it is
for the host tests (HOST_MOCK, code/test/unit/src/nspanel)

*/

#if HOST_MOCK
#include <Arduino.h>
#define NSPANEL_CONFIG_BUILD 1
#else
#include "espurna.h"
#define NSPANEL_CONFIG_BUILD NSPANEL_SUPPORT
#endif

#if NSPANEL_CONFIG_BUILD

#include "nspanel_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace espurna {
namespace nspanel {

// -----------------------------------------------------------------------------
// Text helpers
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

// Sakamoto, 0 = Sunday; -1 for a month outside 1..12 (the table index would be out of bounds)
int weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if ((m < 1) || (m > 12)) {
        return -1;
    }
    if (m < 3) {
        y -= 1;
    }
    return (((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7) + 7) % 7;
}

const char* driverPrefName(DriverPref value) {
    switch (value) {
    case DriverPref::Auto:
        return "auto";
    case DriverPref::Ohpanel:
        return "ohpanel";
    case DriverPref::Lovelace:
        return "lovelace";
    }

    return "";
}

namespace {

// what the running parse checks against, ref. parseInPlace()
Limits limits;

// one entity per (topic, path, on, off, payload); a later "command" replaces the default one
int findOrAddEntity(Config& out, Entity&& want) {
    for (size_t index = 0; index < out.entities.size(); ++index) {
        auto& entity = out.entities[index];
        if ((entity.relay < 0) && (entity.topic == want.topic) && (entity.path == want.path)
            && (entity.on == want.on) && (entity.off == want.off) && (entity.payload == want.payload)
            && (entity.url_on == want.url_on) && (entity.url_off == want.url_off))
        {
            if (want.command.length()) {
                entity.command = want.command;
            }
            return index;
        }
    }

    if (!want.command.length()) {
        want.command = want.topic + out.set;
    }
    out.entities.push_back(std::move(want));

    return out.entities.size() - 1;
}

int findOrAddTopic(Config& out, const String& topic, const String& command) {
    Entity entity;
    entity.topic = topic;
    entity.command = command;
    return findOrAddEntity(out, std::move(entity));
}

// ["a", "b", 0] or "a" -> "a\x1Fb\x1F0"
bool parsePath(const JsonVariant& value, String& out) {
    out = String();
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();  // NULL for a JSON null
        if (!text) {
            return false;
        }
        out = text;
        return out.length() > 0;
    }

    JsonArray& keys = value.as<JsonArray>();
    if (!keys.success() || !keys.size()) {
        return false;
    }

    bool first = true;
    for (auto key : keys) {
        if (!first) {
            out += PathSep;
        }
        first = false;
        if (key.is<const char*>() && key.as<const char*>()) {
            out += key.as<const char*>();
        } else if (key.is<long>()) {
            out += String(key.as<long>());
        } else {
            return false;
        }
    }

    return true;
}

String joinPath(const String& first, const char* second) {
    String out(first);
    out += PathSep;
    out += second;
    return out;
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

// "x" -> x, 12 -> "12", true -> "true", missing -> ""
String jsonText(const JsonVariant& value) {
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();  // NULL for a JSON null
        return text ? String(text) : String();
    }
    if (value.is<bool>()) {
        return value.as<bool>() ? F("true") : F("false");
    }
    if (value.is<long>()) {
        return String(value.as<long>());
    }
    if (value.is<double>()) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%g", value.as<double>());
        return buf;
    }
    return String();
}

// "url" (both), "on_url", "off_url": plain http:// or https:// (no certificate check), empty when not set
bool parseUrls(JsonObject& obj, String& on, String& off, String& error) {
    const String url = jsonText(obj["url"]);
    on = jsonText(obj["on_url"]);
    off = jsonText(obj["off_url"]);
    if (!on.length()) {
        on = url;
    }
    if (!off.length()) {
        off = url;
    }

    for (const auto* value : {&on, &off}) {
        if (value->length() && (!(value->startsWith(F("http://")) || value->startsWith(F("https://")))
            || (value->length() > 256)))
        {
            error = F("\"url\" must start with http:// or https://, at most 256 characters");
            return false;
        }
    }

    return true;
}

bool parseTarget(JsonObject& obj, Config& out, Target& target, String& error) {
    if (!parseAction(obj["action"].as<const char*>(), target.action)) {
        error = F("unknown action '");
        error += obj["action"].as<const char*>();
        error += '\'';
        return false;
    }

    String url_on;
    String url_off;
    if (!parseUrls(obj, url_on, url_off, error)) {
        return false;
    }
    const bool urls = url_on.length() || url_off.length();

    if (urls && obj.containsKey("relay")) {
        error = F("\"url\" and \"relay\" do not go together");
        return false;
    }

    if (obj.containsKey("relay")) {
        const int relay = obj["relay"].as<int>();
        if ((relay < 0) || (static_cast<size_t>(relay) >= limits.relays)) {
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

        Entity entity;
        entity.topic = topic;
        entity.command = jsonText(obj["command"]);
        if (obj.containsKey("path") && !parsePath(obj["path"], entity.path)) {
            error = F("bad \"path\" (array of keys)");
            return false;
        }
        entity.on = jsonText(obj["on"]);
        entity.off = jsonText(obj["off"]);
        entity.payload = jsonText(obj["payload"]);
        entity.url_on = url_on;
        entity.url_off = url_off;
        target.entity = findOrAddEntity(out, std::move(entity));
    } else if (urls) {
        // no state topic: an entity of its own, the panel keeps its state (entitySet())
        Entity entity;
        entity.url_on = url_on;
        entity.url_off = url_off;
        out.entities.push_back(std::move(entity));
        target.entity = out.entities.size() - 1;
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

    // a page index (hardware buttons); an item's "page" object is a nested page (parsePage())
    if (obj["page"].is<int>()) {
        target.page = obj["page"].as<int>();
    }

    target.valid = (target.entity >= 0) || (target.group >= 0) || (target.page >= 0);
    return true;
}

template <typename T>
T clampValue(long value, long min, long max) {
    return static_cast<T>(std::max(min, std::min(max, value)));
}


// "#rrggbb" / "rrggbb" (RGB) or a number (RGB565 decimal)
bool parseColor(const JsonVariant& value, int32_t& out) {
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        if (!text) {
            return false;
        }
        if (text[0] == '#') {
            ++text;
        }
        if (strlen(text) != 6) {
            return false;
        }
        char* end = nullptr;
        const uint32_t rgb = strtoul(text, &end, 16);
        if (!end || *end) {
            return false;
        }
        out = (((rgb >> 19) & 0x1F) << 11) | (((rgb >> 10) & 0x3F) << 5) | ((rgb >> 3) & 0x1F);
        return true;
    }

    if (value.is<long>()) {
        const long number = value.as<long>();
        if ((number >= 0) && (number <= 65535)) {
            out = number;
            return true;
        }
    }

    return false;
}

// "open" references are resolved after all pages are known
struct PendingOpen {
    size_t item;
    String ref;
};

// a JSON value entity of the thermostat: explicit path, or [room, key]; -1 when neither is given
bool thermostatEntity(JsonObject& obj, const char* field, const String& topic, const String& room,
        const char* key, int& out_index, Config& out, String& error)
{
    Entity entity;
    entity.topic = topic;
    if (obj[field].is<double>() || obj[field].is<long>()) {
        out_index = -1; // a fixed number (min / max)
        return true;
    }

    if (obj.containsKey(field)) {
        if (!parsePath(obj[field], entity.path)) {
            error = String(F("bad \"")) + field + F("\" path");
            return false;
        }
    } else if (room.length()) {
        entity.path = joinPath(room, key);
    } else {
        out_index = -1;
        return true;
    }

    out_index = findOrAddEntity(out, std::move(entity));
    return true;
}

// {"type": "thermostat", "topic": "istabai/out", "room": "Гостиная", "command": "istabai/in/Гостиная", "step": 0.5}
// or explicit paths: "current", "setpoint", "humidity", "active", "min", "max" (min / max may be numbers)
bool parseThermostat(JsonObject& obj, Config& out, Item& item, String& error) {
    const char* topic = obj["topic"].as<const char*>();
    if (!topic || !strlen(topic) || strchr(topic, '#') || strchr(topic, '+')) {
        error = F("thermostat needs a state \"topic\"");
        return false;
    }

    const String state_topic(topic);
    const String room = jsonText(obj["room"]);

    Thermostat thermostat;
    if (!thermostatEntity(obj, "current", state_topic, room, "temperature", thermostat.current, out, error)
        || !thermostatEntity(obj, "humidity", state_topic, room, "humidity", thermostat.humidity, out, error)
        || !thermostatEntity(obj, "active", state_topic, room, "active", thermostat.active, out, error)
        || !thermostatEntity(obj, "min", state_topic, room, "min_temperature", thermostat.min, out, error)
        || !thermostatEntity(obj, "max", state_topic, room, "max_temperature", thermostat.max, out, error))
    {
        return false;
    }

    // the setpoint carries the command: plain number unless "payload" has a "%s" template
    {
        Entity entity;
        entity.topic = state_topic;
        if (obj.containsKey("setpoint")) {
            if (!parsePath(obj["setpoint"], entity.path)) {
                error = F("bad \"setpoint\" path");
                return false;
            }
        } else if (room.length()) {
            entity.path = joinPath(room, "set_temperature");
        } else {
            error = F("thermostat needs \"room\" or a \"setpoint\" path");
            return false;
        }
        entity.command = jsonText(obj["command"]);
        entity.payload = jsonText(obj["payload"]);
        thermostat.setpoint = findOrAddEntity(out, std::move(entity));
    }

    if (obj["min"].is<double>() || obj["min"].is<long>()) {
        thermostat.min_value = obj["min"].as<float>();
    }
    if (obj["max"].is<double>() || obj["max"].is<long>()) {
        thermostat.max_value = obj["max"].as<float>();
    }
    if (obj.containsKey("step")) {
        thermostat.step = obj["step"].as<float>();
    }
    if (!(thermostat.step > 0.0f) || (thermostat.step > 10.0f)) {
        thermostat.step = 0.5f;
    }
    if (thermostat.max_value <= thermostat.min_value) {
        thermostat.max_value = thermostat.min_value + thermostat.step;
    }

    const char* history = obj["history"].as<const char*>();
    if (history && strlen(history)) {
        if (strchr(history, '#') || strchr(history, '+')) {
            error = F("\"history\" must be a plain topic");
            return false;
        }
        thermostat.history = history;
    }

    out.thermostats.push_back(thermostat);
    item.thermostat = out.thermostats.size() - 1;
    item.target.entity = thermostat.setpoint;
    item.target.valid = true;
    return true;
}

// a printf format with exactly one float conversion (%f %e %g, flags/width/precision) and %%
bool validFormat(const String& format) {
    size_t conversions = 0;
    for (size_t pos = 0; pos < format.length(); ++pos) {
        if (format[pos] != '%') {
            continue;
        }
        ++pos;
        if ((pos < format.length()) && (format[pos] == '%')) {
            continue;
        }
        while ((pos < format.length()) && strchr("-+ #0", format[pos])) {
            ++pos;
        }
        while ((pos < format.length()) && isdigit(static_cast<unsigned char>(format[pos]))) {
            ++pos;
        }
        if ((pos < format.length()) && (format[pos] == '.')) {
            ++pos;
            while ((pos < format.length()) && isdigit(static_cast<unsigned char>(format[pos]))) {
                ++pos;
            }
        }
        if ((pos >= format.length()) || !strchr("fFeEgG", format[pos])) {
            return false;
        }
        ++conversions;
    }

    return (conversions == 1) && (format.length() <= 24);
}

bool parseItem(JsonObject& item_obj, Config& out, Item& item, std::vector<PendingOpen>& pending,
        const String& where, String& error, String& warnings)
{
    item.name = sanitize(item_obj["name"].as<const char*>(), build::NameMax);

    const char* item_type = item_obj["type"].as<const char*>();
    const bool open = item_obj.containsKey("open") || item_obj.containsKey("page");
    uint16_t fallback = icons::LightSwitch;
    if (open && (!item_type || !strcmp(item_type, "link"))) {
        item.type = ItemType::Link;
        fallback = icons::ArrowRightBold;
    } else if (!item_type || !strcmp(item_type, "switch")) {
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
    } else if (!strcmp(item_type, "thermostat")) {
        item.type = ItemType::Thermostat;
        fallback = icons::Thermometer;
    } else {
        error = where + F(": unknown item type '") + item_type + '\'';
        return false;
    }

    if (item.type == ItemType::Thermostat) {
        if (!parseThermostat(item_obj, out, item, error)) {
            error = where + F(" '") + item.name + F("': ") + error;
            return false;
        }
    } else if (!parseTarget(item_obj, out, item.target, error)) {
        error = where + F(" '") + item.name + F("': ") + error;
        return false;
    }

    // "page": a nested page, target.page is set once it is parsed (parsePage())
    if ((item.type == ItemType::Link) && item_obj.containsKey("page")) {
        if (!item_obj["page"].is<JsonObject&>()) {
            error = where + F(" '") + item.name + F("': \"page\" must be an object");
            return false;
        }
    } else if (item.type == ItemType::Link) {
        const auto& value = item_obj["open"];
        if (value.is<int>()) {
            item.target.page = value.as<int>();
            if ((item.target.page < 0) && (item.target.page != PageNone)) {
                item.target.page = PageNone;
            }
        } else {
            const char* ref = value.as<const char*>();
            if (ref && !strcmp(ref, "home")) {
                item.target.page = PageHome;
            } else if (ref && !strcmp(ref, "weather")) {
                item.target.page = PageWeather;
            } else if (ref && !strcmp(ref, "settings")) {
                item.target.page = PageSettings;
            } else {
                pending.push_back(PendingOpen{out.items.size(), String(ref ? ref : "")});
            }
        }
    } else if (item.type != ItemType::Thermostat) {
        const bool need_group = (item.type == ItemType::Scene);
        if (need_group ? (item.target.group < 0) : (item.target.entity < 0)) {
            error = where + F(" '") + item.name
                + (need_group ? F("': scene needs \"group\"") : F("': needs \"topic\", \"relay\" or \"url\""));
            return false;
        }
    }

    const char* history = item_obj["history"].as<const char*>();
    if ((item.type != ItemType::Thermostat) && history && strlen(history)) {
        if (strchr(history, '#') || strchr(history, '+')) {
            error = where + F(" '") + item.name + F("': \"history\" must be a plain topic");
            return false;
        }
        item.history = history;
    }

    item.icon = resolveIcon(item_obj["icon"].as<const char*>(), fallback, warnings);

    if (item_obj.containsKey("color") && !parseColor(item_obj["color"], item.color)) {
        warnings += where + F(" '") + item.name + F("': bad color ignored; ");
    }

    // same names as AppDaemon's grid 'font' option (pages.py), or the TFT font id 0..9
    if (item_obj.containsKey("font")) {
        const auto& font = item_obj["font"];
        const char* font_name = font.as<const char*>();
        if (font.is<int>() && (font.as<int>() >= 0) && (font.as<int>() <= 9)) {
            item.font = font.as<int>();
        } else if (font_name && !strcmp(font_name, "small")) {
            item.font = 0;
        } else if (font_name && !strcmp(font_name, "medium-icon")) {
            item.font = 1;
        } else if (font_name && !strcmp(font_name, "medium")) {
            item.font = 2;
        } else if (font_name && !strcmp(font_name, "large")) {
            item.font = 3;
        } else {
            warnings += F("item '");
            warnings += item.name;
            warnings += F("': unknown font ignored; ");
        }
    }

    return true;
}

// one item ("type" counted for thermostats) and its nested page, for the vector reservations below
void countItem(JsonObject& obj, size_t& items, size_t& thermostats, size_t& pages, int depth = 0) {
    ++items;
    const char* type = obj["type"].as<const char*>();
    if (type && !strcmp(type, "thermostat")) {
        ++thermostats;
    }

    JsonObject& page = obj["page"];
    if (page.success() && (depth < 8)) {
        ++pages;
        JsonArray& nested = page["items"];
        for (auto item_variant : nested) {
            JsonObject& item = item_variant.as<JsonObject>();
            if (item.success()) {
                countItem(item, items, thermostats, pages, depth + 1);
            }
        }
    }
}

// an item's "page", parsed after all top-level pages (their indices stay those of "pages")
struct PendingPage {
    JsonObject* obj;
    size_t item;
};

bool parsePage(JsonObject& page_obj, Config& out, std::vector<PendingOpen>& pending,
        std::vector<PendingPage>& nested, size_t page_index, const String& fallback_title,
        String& error, String& warnings)
{
    const char* type = page_obj["type"].as<const char*>();
    Page page;
    if (type && !strcmp(type, "settings")) {
        page.settings = true;
    } else if (type && !strcmp(type, "list")) {
        page.list = true;
    } else if (type && strcmp(type, "grid")) {
        warnings += F("page type '");
        warnings += type;
        warnings += F("' shown as grid; ");
    }

    page.id = page_obj["id"].as<const char*>();
    if (!page.id.length()) {
        page.id = String('p') + page_index;
    }
    page.title = sanitize(page_obj["title"].as<const char*>(), build::HeadingMax);
    if (!page.title.length()) {
        page.title = sanitize(fallback_title.c_str(), build::HeadingMax);
    }

    if (page_obj.containsKey("columns")) {
        const int columns = page_obj["columns"].as<int>();
        if ((columns == 3) || (columns == 4)) {
            page.grid = (columns == 3) ? Grid::G3x2 : Grid::G4x2;
        } else {
            warnings += F("page ");
            warnings += page_index;
            warnings += F(": columns must be 3 or 4; ");
        }
    }

    // "layout" wins over "columns"
    if (page_obj.containsKey("layout")) {
        const char* layout = page_obj["layout"].as<const char*>();
        bool found = false;
        for (size_t index = 0; index < (sizeof(GridNames) / sizeof(GridNames[0])); ++index) {
            if (layout && !strcmp(layout, GridNames[index])) {
                page.grid = static_cast<Grid>(index);
                found = true;
            }
        }
        if (!found) {
            warnings += F("page ");
            warnings += page_index;
            warnings += F(": layout must be 4x2, 3x3, 3x2, 2x2 or 1x2; ");
        }
    }

    if (page_obj.containsKey("per_screen")) {
        const int per_screen = page_obj["per_screen"].as<int>();
        if ((per_screen >= 1) && (per_screen <= static_cast<int>(TilesMax))) {
            page.per_screen = per_screen;
        } else {
            warnings += F("page ");
            warnings += page_index;
            warnings += F(": per_screen must be 1..9; ");
        }
    }

    if (page_obj.containsKey("draw")) {
        page.draw = page_obj["draw"].as<bool>();
    }

    JsonArray& items = page_obj["items"];
    if (page.settings && items.success() && items.size()) {
        warnings += F("settings page: items ignored; ");
    } else if (items.success()) {
        page.items.reserve(items.size());
        const String where = String(F("page ")) + page_index + F(" item");
        for (auto item_variant : items) {
            JsonObject& item_obj = item_variant.as<JsonObject>();
            if (!item_obj.success()) {
                error = F("item is not an object");
                return false;
            }

            Item item;
            if (!parseItem(item_obj, out, item, pending, where, error, warnings)) {
                return false;
            }

            out.items.push_back(std::move(item));
            page.items.push_back(out.items.size() - 1);

            JsonObject& sub = item_obj["page"];
            if (sub.success() && (out.items.back().type == ItemType::Link)) {
                nested.push_back(PendingPage{&sub, out.items.size() - 1});
            } else if (sub.success()) {
                warnings += where + F(" '") + out.items.back().name + F("': \"page\" needs no \"type\", ignored; ");
            }
        }
    }

    out.pages.push_back(std::move(page));
    return true;
}

bool parseRoot(JsonObject& root, Config& out, String& error, String& warnings) {
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
        // the NTC is an ESPurna sensor: its correction and MQTT interval are the sensor settings
        if (temperature.containsKey("offset") || temperature.containsKey("publish")) {
            warnings += F("temperature: offset and publish are ignored, use the Sensors settings of the web UI; ");
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

    if (root.containsKey("display")) {
        const char* display = root["display"].as<const char*>();
        if (display && !strcmp(display, "ohpanel")) {
            out.display = DriverPref::Ohpanel;
        } else if (display && !strcmp(display, "lovelace")) {
            out.display = DriverPref::Lovelace;
        } else if (!display || strcmp(display, "auto")) {
            warnings += F("display: auto, ohpanel or lovelace (using auto); ");
        }
    }

    if (root.containsKey("language")) {
        const char* language = root["language"].as<const char*>();
        if (language && !strcmp(language, "ru")) {
            out.language = lang::Language::Russian;
        } else if (!language || strcmp(language, "en")) {
            warnings += F("language: en or ru (using en); ");
        }
    }

    JsonObject& ohpanel = root["ohpanel"];
    if (ohpanel.success()) {
        if (ohpanel.containsKey("background")) {
            const char* name = ohpanel["background"].as<const char*>();
            bool found = false;
            for (size_t index = 0; index < (sizeof(BackgroundNames) / sizeof(BackgroundNames[0])); ++index) {
                if (name && !strcmp(name, BackgroundNames[index])) {
                    out.background = index;
                    found = true;
                }
            }
            if (!found) {
                warnings += F("ohpanel.background: aurora, black, candy, dunes or forest; ");
            }
        }

        if (ohpanel.containsKey("accent") && !parseColor(ohpanel["accent"], out.accent)) {
            warnings += F("ohpanel.accent: bad colour; ");
        }
        if (ohpanel.containsKey("text") && !parseColor(ohpanel["text"], out.text_color)) {
            warnings += F("ohpanel.text: bad colour; ");
        }
        if (ohpanel.containsKey("dim") && !parseColor(ohpanel["dim"], out.dim_color)) {
            warnings += F("ohpanel.dim: bad colour; ");
        }
        if (ohpanel.containsKey("weather_days")) {
            out.weather_days = clampValue<uint8_t>(ohpanel["weather_days"].as<long>(), 0, 7);
        }

        if (ohpanel.containsKey("layout")) {
            const char* layout = ohpanel["layout"].as<const char*>();
            if (layout && !strcmp(layout, "corner")) {
                out.layout = Layout::Corner;
            } else if (layout && !strcmp(layout, "center")) {
                out.layout = Layout::Center;
            } else {
                warnings += F("ohpanel.layout: center or corner; ");
            }
        }

        if (ohpanel.containsKey("theme")) {
            const char* theme = ohpanel["theme"].as<const char*>();
            if (theme && !strcmp(theme, "light")) {
                out.theme = Theme::Light;
            } else if (theme && !strcmp(theme, "dark")) {
                out.theme = Theme::Dark;
            } else {
                warnings += F("ohpanel.theme: dark or light; ");
            }
        }

        JsonArray& relays = ohpanel["relays"];
        if (relays.success()) {
            for (size_t index = 0; (index < 2) && (index < relays.size()); ++index) {
                out.relay_labels[index] = sanitize(relays[index].as<const char*>(), 26);
            }
        }
    }

    JsonObject& groups = root["groups"];
    if (groups.success()) {
        out.group_names.reserve(groups.size());
        out.groups.reserve(groups.size());
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
                    if ((relay >= 0) && (static_cast<size_t>(relay) < limits.relays)) {
                        entities.push_back(findOrAddRelay(out, relay));
                        continue;
                    }
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

    // exact sizes up front: no vector doubling (old + new block alive while it moves)
    {
        size_t item_count = 0;
        size_t thermostat_count = 0;
        size_t page_count = pages.size();
        for (auto page_variant : pages) {
            JsonArray& items = page_variant.as<JsonObject>()["items"];
            for (auto item_variant : items) {
                JsonObject& obj = item_variant.as<JsonObject>();
                if (obj.success()) {
                    countItem(obj, item_count, thermostat_count, page_count);
                }
            }
        }

        JsonArray& shortcuts = screensaver["shortcuts"];
        for (auto shortcut : shortcuts) {
            JsonObject& obj = shortcut.as<JsonObject>();
            if (obj.success() && !obj.containsKey("ref")) {
                countItem(obj, item_count, thermostat_count, page_count);
            }
        }
        out.shortcuts.reserve(std::min<size_t>(7, shortcuts.size()));

        out.items.reserve(item_count);
        out.thermostats.reserve(thermostat_count);
        out.pages.reserve(page_count);
    }

    std::vector<PendingOpen> pending;

    std::vector<PendingPage> nested;
    size_t page_index = 0;
    for (auto page_variant : pages) {
        JsonObject& page_obj = page_variant.as<JsonObject>();
        if (!page_obj.success()) {
            error = F("page is not an object");
            return false;
        }
        if (!parsePage(page_obj, out, pending, nested, page_index, String(), error, warnings)) {
            return false;
        }
        ++page_index;
    }

    // nested pages after the swipe row, breadth first (a sub-page may hold further sub-pages)
    for (size_t index = 0; index < nested.size(); ++index) {
        const auto job = nested[index];
        if (!parsePage(*job.obj, out, pending, nested, out.pages.size(), out.items[job.item].name, error, warnings)) {
            return false;
        }
        out.pages.back().nested = true;
        out.items[job.item].target.page = out.pages.size() - 1;
    }

    // home shortcuts (OHpanel, up to 7): an item name, {"ref": "<item name>"} or a full item
    JsonArray& shortcuts = screensaver["shortcuts"];
    if (shortcuts.success()) {
        for (auto shortcut : shortcuts) {
            if (out.shortcuts.size() >= 7) {
                warnings += F("more than 7 shortcuts, rest ignored; ");
                break;
            }

            JsonObject& obj = shortcut.as<JsonObject>();
            const char* ref = shortcut.is<const char*>() ? shortcut.as<const char*>()
                : (obj.success() ? obj["ref"].as<const char*>() : nullptr);
            if (ref) {
                bool found = false;
                for (size_t index = 0; index < out.items.size(); ++index) {
                    if (out.items[index].name == ref) {
                        out.shortcuts.push_back(index);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    warnings += F("shortcut '");
                    warnings += ref;
                    warnings += F("': no such item; ");
                }
                continue;
            }

            if (!obj.success()) {
                error = F("shortcut is not an object or an item name");
                return false;
            }

            Item item;
            if (!parseItem(obj, out, item, pending, String(F("shortcut")), error, warnings)) {
                return false;
            }
            out.items.push_back(std::move(item));
            out.shortcuts.push_back(out.items.size() - 1);
        }
    }

    // home info lines (OHpanel info1 / info2): icon, text, a value from a topic (+ an extra value)
    JsonArray& info = screensaver["info"];
    if (info.success()) {
        for (auto line_variant : info) {
            if (out.info_count >= InfoLinesMax) {
                warnings += F("more than 2 info lines, rest ignored; ");
                break;
            }

            JsonObject& obj = line_variant.as<JsonObject>();
            if (!obj.success()) {
                error = F("info line is not an object");
                return false;
            }

            auto& line = out.info[out.info_count];
            const String where = String(F("info ")) + (out.info_count + 1);
            const char* icon = obj["icon"].as<const char*>();
            line.icon = (icon && strlen(icon)) ? resolveIcon(icon, 0, warnings) : String();
            line.text = sanitize(obj["text"].as<const char*>(), 31);

            const char* topic = obj["topic"].as<const char*>();
            if (topic && strlen(topic)) {
                if (strchr(topic, '#') || strchr(topic, '+')) {
                    error = where + F(": invalid topic");
                    return false;
                }

                Entity entity;
                entity.topic = topic;
                if (obj.containsKey("path") && !parsePath(obj["path"], entity.path)) {
                    error = where + F(": bad \"path\"");
                    return false;
                }
                line.entity = findOrAddEntity(out, std::move(entity));
                line.format = jsonText(obj["format"]);

                JsonObject& extra = obj["extra"];
                if (extra.success()) {
                    Entity second;
                    const char* extra_topic = extra["topic"].as<const char*>();
                    second.topic = (extra_topic && strlen(extra_topic)) ? String(extra_topic) : String(topic);
                    if (extra.containsKey("path") && !parsePath(extra["path"], second.path)) {
                        error = where + F(": bad extra \"path\"");
                        return false;
                    }
                    line.extra = findOrAddEntity(out, std::move(second));
                    line.extra_format = jsonText(extra["format"]);
                }
            }

            if (line.format.length() && !validFormat(line.format)) {
                warnings += where + F(": format needs one %f / %g conversion, ignored; ");
                line.format = String();
            }
            if (line.extra_format.length() && !validFormat(line.extra_format)) {
                warnings += where + F(": extra format needs one %f / %g conversion, ignored; ");
                line.extra_format = String();
            }

            ++out.info_count;
        }
    }

    for (const auto& open : pending) {
        auto& target = out.items[open.item].target;
        for (size_t index = 0; index < out.pages.size(); ++index) {
            if ((out.pages[index].id == open.ref) || (out.pages[index].title == open.ref)) {
                target.page = index;
                break;
            }
        }
        if (target.page == PageNone) {
            error = String(F("item '")) + out.items[open.item].name + F("': unknown page '") + open.ref + '\'';
            return false;
        }
    }

    for (auto& item : out.items) {
        if ((item.type == ItemType::Link) && (item.target.page >= static_cast<int>(out.pages.size()))) {
            error = String(F("item '")) + item.name + F("': no page ") + item.target.page;
            return false;
        }
        // an "open" tile without "name" shows the title of its page
        if ((item.type == ItemType::Link) && (item.target.page >= 0) && !item.name.length()) {
            item.name = sanitize(out.pages[item.target.page].title.c_str(), build::NameMax);
        }
    }
    JsonArray& buttons = root["buttons"];
    if (buttons.success()) {
        out.buttons.reserve(buttons.size());
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

    // mqtt.cpp drops MQTT_MESSAGE_EVENT payloads of MQTT_BUFFER_MAX_SIZE bytes and more
    // (Istabai istabai/out with 9 rooms is ~1.3 KB)
    if (limits.mqtt_buffer && !out.thermostats.empty() && (limits.mqtt_buffer < 2048)) {
        warnings += F("MQTT_BUFFER_MAX_SIZE is ");
        warnings += String(static_cast<unsigned>(limits.mqtt_buffer));
        warnings += F(": longer state payloads are dropped by the MQTT module; ");
    }

    return true;
}

// ArduinoJson 5 parses a mutable char* in place (strings stay in the text, nothing is copied)
// into one block that we size and allocate ourselves. DynamicJsonBuffer(1024) used to copy all
// strings (the text was const) into blocks doubling from 1 KB: ~15 KB for a 7.4 KB config
struct HeapJsonBuffer : public ArduinoJson::Internals::StaticJsonBufferBase {
    HeapJsonBuffer(char* buffer, size_t capacity) :
        StaticJsonBufferBase(buffer, capacity)
    {}
};

} // namespace

// Exact node storage of an in-place parse: every object, pair, array and array element.
// Unknown syntax (comments) falls back to counting every comma as an array element
size_t jsonBufferSize(const char* text, size_t length) {
    size_t objects = 0;
    size_t arrays = 0;
    size_t pairs = 0;
    size_t elements = 0;
    size_t commas = 0;
    bool exact = true;

    uint32_t is_array = 0; // per nesting level
    size_t depth = 0;
    bool first = false;    // right after '[': the next token is the first element
    char quote = '\0';
    bool escape = false;

    for (size_t pos = 0; pos < length; ++pos) {
        const char c = text[pos];
        if (quote) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }

        if ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n')) {
            continue;
        }

        if (first) {
            first = false;
            if (c != ']') {
                ++elements;
            }
        }

        switch (c) {
        case '"':
        case '\'':
            quote = c;
            break;
        case '{':
        case '[':
            if (depth >= 31) {
                exact = false;
                break;
            }
            ++depth;
            if (c == '[') {
                ++arrays;
                is_array |= (1u << depth);
                first = true;
            } else {
                ++objects;
                is_array &= ~(1u << depth);
            }
            break;
        case '}':
        case ']':
            if (depth) {
                --depth;
            }
            break;
        case ':':
            ++pairs;
            break;
        case ',':
            ++commas;
            if (is_array & (1u << depth)) {
                ++elements;
            }
            break;
        case '/':
            exact = false;
            break;
        default:
            break;
        }
    }

    if (!exact) {
        elements = commas + arrays;
    }

    // JSON_*_SIZE() are multiples of the 4 byte alignment the buffer rounds every allocation to
    return (objects * JSON_OBJECT_SIZE(0))
        + (pairs * (JSON_OBJECT_SIZE(1) - JSON_OBJECT_SIZE(0)))
        + (arrays * JSON_ARRAY_SIZE(0))
        + (elements * (JSON_ARRAY_SIZE(1) - JSON_ARRAY_SIZE(0)))
        + 64;
}

// 'text' is NUL terminated and destroyed by the in-place parse; 'out' is garbage unless Ok.
// No exception leaves here (-fexceptions build: std::vector / std::deque throw bad_alloc)
ParseResult parseInPlace(char* text, size_t json_size, const Limits& with, Config& out,
        String& error, String& warnings)
{
    limits = with;

    std::unique_ptr<char[]> nodes(new (std::nothrow) char[json_size]);
    if (!nodes) {
        error = F("out of memory (JSON), retry");
        return ParseResult::LowMemory;
    }

    try {
        HeapJsonBuffer buffer(nodes.get(), json_size);
        JsonObject& root = buffer.parseObject(text, build::ConfigNestingLimit);
        if (!root.success()) {
            error = ((buffer.size() + JSON_OBJECT_SIZE(1)) > json_size)
                ? F("JSON nesting / size not supported")
                : F("invalid JSON");
            return ParseResult::Invalid;
        }

        return parseRoot(root, out, error, warnings) ? ParseResult::Ok : ParseResult::Invalid;
    } catch (const std::bad_alloc&) {
        error = F("out of memory, retry");
    }

    return ParseResult::LowMemory;
}

size_t iconCount() {
    return sizeof(icons::Table) / sizeof(icons::Table[0]);
}

const char* iconName(size_t index) {
    return (index < iconCount()) ? icons::Table[index].name : nullptr;
}

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_CONFIG_BUILD
