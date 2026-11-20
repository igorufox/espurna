/*

NSPANEL MODULE - config compiler

/nspanel.json -> the image of nspanel_config.h, and the image's tree -> JSON again. Built for the firmware
(NSPANEL_SUPPORT) and as it is for the host tests (HOST_MOCK, code/test/unit/src/nspanel)

*/

#if HOST_MOCK
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
#include <deque>
#include <memory>
#include <new>

namespace espurna {
namespace nspanel {

// -----------------------------------------------------------------------------
// Text helpers
// -----------------------------------------------------------------------------

espurna::string utf8(uint16_t code) {
    espurna::string out;
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
void utf8Truncate(espurna::string& value, size_t max) {
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
espurna::string utf8Prefix(const espurna::string& value, size_t chars) {
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
espurna::string sanitize(const char* value, size_t max) {
    espurna::string out(value ? value : "");
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

// -----------------------------------------------------------------------------
// The draft: what the parse builds before the image is laid out. Texts are offsets into one de-duplicated string
// table, so equal texts are equal offsets. Nothing of it is one big heap block (a fragmented heap with Matter running,
// 11-16): the string table and the tree grow in blocks of build::CompileBlock, the record lists are deques (blocks of
// 512 bytes)
// -----------------------------------------------------------------------------

using Off = uint16_t;

template <typename T>
using List = std::deque<T>;

// The string table in blocks: a text never spans two blocks (a longer one gets a block of its own size), the offsets
// run on from block to block as the image lays the used bytes one after the other
struct Strings {
    struct Block {
        std::unique_ptr<char[]> data;
        size_t base;     // offset of data[0] in the table
        size_t used;
        size_t capacity;
    };

    std::vector<Block> blocks;
    size_t total { 0 };
    bool overflow { false };
    bool failed { false };   // out of memory

    Strings() {
        // offset 0: ""
        if (grow(1)) {
            blocks.back().data[0] = '\0';
            blocks.back().used = 1;
            total = 1;
        }
    }

    Off add(const char* text) {
        const size_t length = text ? strlen(text) : 0;
        if (!length) {
            return 0;
        }

        for (const auto& block : blocks) {
            for (size_t at = 0; at < block.used;) {
                const char* existing = block.data.get() + at;
                const size_t size = strlen(existing);
                if ((size == length) && (0 == std::memcmp(existing, text, length))) {
                    return block.base + at;
                }
                at += size + 1;
            }
        }

        if ((total + length + 1) > build::ConfigMax) {
            overflow = true;
            return 0;
        }

        if (blocks.empty() || ((blocks.back().capacity - blocks.back().used) < (length + 1))) {
            if (!grow(length + 1)) {
                failed = true;
                return 0;
            }
        }

        auto& block = blocks.back();
        const Off out = block.base + block.used;
        std::memcpy(block.data.get() + block.used, text, length + 1);
        block.used += length + 1;
        total += length + 1;
        return out;
    }

    Off add(const espurna::string& text) {
        return add(text.c_str());
    }

    const char* at(Off offset) const {
        for (const auto& block : blocks) {
            if ((offset >= block.base) && (offset < (block.base + block.used))) {
                return block.data.get() + (offset - block.base);
            }
        }
        return "";
    }

    size_t size() const {
        return total;
    }

    bool grow(size_t need) {
        Block block;
        block.capacity = std::max(need, build::CompileBlock);
        block.data.reset(new (std::nothrow) char[block.capacity]);
        if (!block.data) {
            failed = true;
            return false;
        }
        block.base = total;
        block.used = 0;
        blocks.push_back(std::move(block));
        return true;
    }
};

// bytes appended in blocks of build::CompileBlock (the tree)
struct Bytes {
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    size_t total { 0 };
    bool failed { false };   // out of memory

    void push_back(uint8_t value) {
        if (total == (blocks.size() * build::CompileBlock)) {
            std::unique_ptr<uint8_t[]> block(new (std::nothrow) uint8_t[build::CompileBlock]);
            if (!block) {
                failed = true;
                return;
            }
            blocks.push_back(std::move(block));
        }
        blocks.back()[total % build::CompileBlock] = value;
        ++total;
    }

    void set(size_t index, uint8_t value) {
        if (index < total) {
            blocks[index / build::CompileBlock][index % build::CompileBlock] = value;
        }
    }

    size_t size() const {
        return total;
    }
};

struct DraftEntity {
    Off topic { 0 };
    Off command { 0 };
    int16_t path { -1 };   // Draft::paths, -1 - none
    Off on { 0 };
    Off off { 0 };
    Off payload { 0 };
    Off url_on { 0 };
    Off url_off { 0 };
    int16_t relay { -1 };
};

struct DraftThermostat {
    int16_t current { -1 };
    int16_t setpoint { -1 };
    int16_t humidity { -1 };
    int16_t active { -1 };
    int16_t min { -1 };
    int16_t max { -1 };
    float min_value { 5.0f };
    float max_value { 30.0f };
    float step { 0.5f };
    Off history { 0 };
};

struct DraftInfo {
    Off icon { 0 };
    Off text { 0 };
    int16_t entity { -1 };
    Off format { 0 };
    int16_t extra { -1 };
    Off extra_format { 0 };
};

struct DraftItem {
    Off name { 0 };
    Off icon { 0 };
    Off history { 0 };
    ItemType type { ItemType::Switch };
    uint8_t font { NoFont };
    int32_t color { -1 };
    int16_t thermostat { -1 };
    Off action { 0 };
    Target target;
};

struct DraftPage {
    Off id { 0 };
    Off title { 0 };
    bool settings { false };
    bool list { false };
    Grid grid { Grid::G4x2 };
    uint8_t per_screen { 0 };
    bool draw { false };
    bool nested { false };
    std::vector<uint16_t> items;
};

struct DraftGroup {
    Off name { 0 };
    std::vector<int16_t> members;
};

struct Draft {
    Strings strings;

    Off on { 0 };
    Off off { 0 };
    Off toggle { 0 };
    Off set { 0 };
    Off relay_labels[2] { 0, 0 };

    uint16_t timeout { 20 };
    uint8_t brightness { 100 };
    uint8_t dim { 20 };

    bool weather { false };
    float lat { 0.0f };
    float lon { 0.0f };
    uint32_t refresh { 900 };

    bool temperature { true };
    bool temperature_show { true };

    bool beep { false };
    bool debug { false };

    uint16_t color_on { icons::colors::On };
    uint16_t color_off { icons::colors::Off };
    uint16_t color_unknown { icons::colors::Unknown };

    DriverPref display { DriverPref::Auto };
    lang::Language language { lang::Language::English };

    uint8_t background { 0 };
    int32_t accent { -1 };
    int32_t text_color { -1 };
    int32_t dim_color { -1 };
    uint8_t weather_days { 7 };
    Layout layout { Layout::Center };
    Theme theme { Theme::Dark };

    std::vector<uint16_t> shortcuts;
    DraftInfo info[InfoLinesMax];
    size_t info_count { 0 };
    List<DraftThermostat> thermostats;
    List<DraftEntity> entities;
    List<DraftGroup> groups;
    List<DraftItem> items;
    List<DraftPage> pages;
    std::vector<Target> buttons;
    List<std::vector<Off>> paths;

    // the accepted JSON (writeJson()): [u16 strings offset (set when laid out)][u16 root], then the nodes
    Bytes tree;
    uint16_t tree_root { 0 };

    Draft() {
        on = strings.add("1");
        off = strings.add("0");
        toggle = strings.add("2");
        set = strings.add("/set");
    }
};

// what the running parse checks against, ref. compile()
Limits limits;

// one entity per (topic, path, on, off, payload); a later "command" replaces the default one
int findOrAddEntity(Draft& out, DraftEntity want) {
    for (size_t index = 0; index < out.entities.size(); ++index) {
        auto& entity = out.entities[index];
        if ((entity.relay < 0) && (entity.topic == want.topic) && (entity.path == want.path)
            && (entity.on == want.on) && (entity.off == want.off) && (entity.payload == want.payload)
            && (entity.url_on == want.url_on) && (entity.url_off == want.url_off))
        {
            if (want.command) {
                entity.command = want.command;
            }
            return index;
        }
    }

    // no "command": the state topic + mqtt.set, made when it is sent (entityCommand())
    out.entities.push_back(want);

    return out.entities.size() - 1;
}

int findOrAddTopic(Draft& out, Off topic) {
    DraftEntity entity;
    entity.topic = topic;
    return findOrAddEntity(out, entity);
}

// equal key lists share one path
int16_t addPath(Draft& out, std::vector<Off>&& keys) {
    for (size_t index = 0; index < out.paths.size(); ++index) {
        if (out.paths[index] == keys) {
            return index;
        }
    }

    out.paths.push_back(std::move(keys));
    return out.paths.size() - 1;
}

// ["a", "b", 0] or "a" -> the keys "a", "b", "0"
// The texts of the JSON as C strings for the parsers below: copies, kept until the compile ends (CstrArena). A string
// without its quotes and escapes, a number or true / false as written; nullptr for null, an object or an array (the
// ArduinoJson 5 as<const char*>() these parsers were written for)
std::vector<espurna::string>* cstr_arena { nullptr };

struct CstrArena {
    CstrArena() {
        cstr_arena = &texts;
    }

    ~CstrArena() {
        cstr_arena = nullptr;
    }

    std::vector<espurna::string> texts;
};

const char* cstr(espurna::string&& text) {
    if (!cstr_arena) {
        return nullptr;
    }

    cstr_arena->push_back(std::move(text));
    const char* out = cstr_arena->back().c_str();
    return out ? out : "";
}

const char* cstr(espurna::json::Value value) {
    if (!value.isString() && !value.isNumber() && !value.isBool()) {
        return nullptr;
    }

    return cstr(value.asString());
}

const char* cstr(espurna::StringView text) {
    return cstr(espurna::string(text));
}

bool parsePath(Draft& out, espurna::json::Value value, int16_t& path) {
    std::vector<Off> keys;
    if (value.isString()) {
        const char* text = cstr(value);  // NULL for a JSON null
        if (!text || !strlen(text)) {
            return false;
        }
        keys.push_back(out.strings.add(text));
        path = addPath(out, std::move(keys));
        return true;
    }

    const auto array = value;
    if (!array.valid() || !array.size()) {
        return false;
    }

    keys.reserve(array.size());
    for (auto key : array) {
        if (key.isString() && cstr(key)) {
            keys.push_back(out.strings.add(cstr(key)));
        } else if (key.isNumber()) {
            keys.push_back(out.strings.add(espurna::string(key.asLong())));
        } else {
            return false;
        }
    }

    path = addPath(out, std::move(keys));
    return true;
}

// [room, key]
int16_t roomPath(Draft& out, const espurna::string& room, const char* key) {
    std::vector<Off> keys;
    keys.push_back(out.strings.add(room));
    keys.push_back(out.strings.add(key));
    return addPath(out, std::move(keys));
}

int findOrAddRelay(Draft& out, int relay) {
    for (size_t index = 0; index < out.entities.size(); ++index) {
        if (out.entities[index].relay == relay) {
            return index;
        }
    }

    DraftEntity entity;
    entity.relay = relay;
    out.entities.push_back(entity);

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

espurna::string resolveIcon(const char* value, uint16_t fallback, espurna::string& warnings) {
    if (!value || !strlen(value)) {
        return utf8(fallback);
    }

    // literal glyph or short text
    if (static_cast<uint8_t>(value[0]) >= 0x80) {
        espurna::string out(value);
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

    warnings += "unknown icon '";
    warnings += value;
    warnings += "'; ";

    return utf8(icons::AlertCircleOutline);
}

// "x" -> x, 12 -> "12", true -> "true", missing -> ""
espurna::string jsonText(espurna::json::Value value) {
    if (value.isString()) {
        const char* text = cstr(value);  // NULL for a JSON null
        return text ? espurna::string(text) : espurna::string();
    }
    if (value.isBool()) {
        return value.asBool() ? "true" : "false";
    }
    if (value.isNumber()) {
        return espurna::string(value.asLong());
    }
    if (value.isNumber()) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%g", value.asDouble());
        return buf;
    }
    return espurna::string();
}

// "url" (both), "on_url", "off_url": plain http:// or https:// (no certificate check), empty when not set
bool parseUrls(espurna::json::Value obj, espurna::string& on, espurna::string& off, espurna::string& error) {
    const espurna::string url = jsonText(obj["url"]);
    on = jsonText(obj["on_url"]);
    off = jsonText(obj["off_url"]);
    if (!on.length()) {
        on = url;
    }
    if (!off.length()) {
        off = url;
    }

    for (const auto* value : {&on, &off}) {
        if (value->length() && (!(value->startsWith("http://") || value->startsWith("https://"))
            || (value->length() > 256)))
        {
            error = "\"url\" must start with http:// or https://, at most 256 characters";
            return false;
        }
    }

    return true;
}

// 'module_action': the item's "action" is the name of another module's action (parseItem()), not toggle / on / off
bool parseTarget(espurna::json::Value obj, Draft& out, Target& target, espurna::string& error, bool module_action = false) {
    if (!module_action && !parseAction(cstr(obj["action"]), target.action)) {
        error = "unknown action '";
        error += cstr(obj["action"]);
        error += '\'';
        return false;
    }

    espurna::string url_on;
    espurna::string url_off;
    if (!parseUrls(obj, url_on, url_off, error)) {
        return false;
    }
    const bool urls = url_on.length() || url_off.length();

    if (urls && obj.has("relay")) {
        error = "\"url\" and \"relay\" do not go together";
        return false;
    }

    if (obj.has("relay")) {
        const int relay = static_cast<int>(obj["relay"].asLong());
        if ((relay < 0) || (static_cast<size_t>(relay) >= limits.relays)) {
            error = "no such relay ";
            error += relay;
            return false;
        }
        target.entity = findOrAddRelay(out, relay);
    } else if (obj.has("topic")) {
        const char* topic = cstr(obj["topic"]);
        if (!topic || !strlen(topic) || strchr(topic, '#') || strchr(topic, '+')) {
            error = "invalid topic";
            return false;
        }

        DraftEntity entity;
        entity.topic = out.strings.add(topic);
        entity.command = out.strings.add(jsonText(obj["command"]));
        if (obj.has("path") && !parsePath(out, obj["path"], entity.path)) {
            error = "bad \"path\" (array of keys)";
            return false;
        }
        entity.on = out.strings.add(jsonText(obj["on"]));
        entity.off = out.strings.add(jsonText(obj["off"]));
        entity.payload = out.strings.add(jsonText(obj["payload"]));
        entity.url_on = out.strings.add(url_on);
        entity.url_off = out.strings.add(url_off);
        target.entity = findOrAddEntity(out, entity);
    } else if (urls) {
        // no state topic: an entity of its own, the panel keeps its state (entitySet())
        DraftEntity entity;
        entity.url_on = out.strings.add(url_on);
        entity.url_off = out.strings.add(url_off);
        out.entities.push_back(entity);
        target.entity = out.entities.size() - 1;
    }

    if (obj.has("group")) {
        const char* name = cstr(obj["group"]);
        for (size_t index = 0; index < out.groups.size(); ++index) {
            if (name && !strcmp(out.strings.at(out.groups[index].name), name)) {
                target.group = index;
                break;
            }
        }

        if (target.group < 0) {
            error = "unknown group '";
            error += name ? name : "";
            error += '\'';
            return false;
        }
    }

    // a page index (hardware buttons); an item's "page" object is a nested page (parsePage())
    if (obj["page"].isNumber()) {
        target.page = static_cast<int>(obj["page"].asLong());
    }

    target.valid = (target.entity >= 0) || (target.group >= 0) || (target.page >= 0);
    return true;
}

template <typename T>
T clampValue(long value, long min, long max) {
    return static_cast<T>(std::max(min, std::min(max, value)));
}


// "#rrggbb" / "rrggbb" (RGB) or a number (RGB565 decimal)
bool parseColor(espurna::json::Value value, int32_t& out) {
    if (value.isString()) {
        const char* text = cstr(value);
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

    if (value.isNumber()) {
        const long number = value.asLong();
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
    espurna::string ref;
};

// a JSON value entity of the thermostat: explicit path, or [room, key]; -1 when neither is given
bool thermostatEntity(espurna::json::Value obj, const char* field, Off topic, const espurna::string& room,
        const char* key, int16_t& out_index, Draft& out, espurna::string& error)
{
    DraftEntity entity;
    entity.topic = topic;
    if (obj[field].isNumber() || obj[field].isNumber()) {
        out_index = -1; // a fixed number (min / max)
        return true;
    }

    if (obj.has(field)) {
        if (!parsePath(out, obj[field], entity.path)) {
            error = espurna::string("bad \"") + field + "\" path";
            return false;
        }
    } else if (room.length()) {
        entity.path = roomPath(out, room, key);
    } else {
        out_index = -1;
        return true;
    }

    out_index = findOrAddEntity(out, entity);
    return true;
}

// {"type": "thermostat", "topic": "istabai/out", "room": "Гостиная", "command": "istabai/in/Гостиная", "step": 0.5}
// or explicit paths: "current", "setpoint", "humidity", "active", "min", "max" (min / max may be numbers)
bool parseThermostat(espurna::json::Value obj, Draft& out, DraftItem& item, espurna::string& error) {
    const char* topic = cstr(obj["topic"]);
    if (!topic || !strlen(topic) || strchr(topic, '#') || strchr(topic, '+')) {
        error = "thermostat needs a state \"topic\"";
        return false;
    }

    const Off state_topic = out.strings.add(topic);
    const espurna::string room = jsonText(obj["room"]);

    DraftThermostat thermostat;
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
        DraftEntity entity;
        entity.topic = state_topic;
        if (obj.has("setpoint")) {
            if (!parsePath(out, obj["setpoint"], entity.path)) {
                error = "bad \"setpoint\" path";
                return false;
            }
        } else if (room.length()) {
            entity.path = roomPath(out, room, "set_temperature");
        } else {
            error = "thermostat needs \"room\" or a \"setpoint\" path";
            return false;
        }
        entity.command = out.strings.add(jsonText(obj["command"]));
        entity.payload = out.strings.add(jsonText(obj["payload"]));
        thermostat.setpoint = findOrAddEntity(out, entity);
    }

    if (obj["min"].isNumber() || obj["min"].isNumber()) {
        thermostat.min_value = obj["min"].asDouble();
    }
    if (obj["max"].isNumber() || obj["max"].isNumber()) {
        thermostat.max_value = obj["max"].asDouble();
    }
    if (obj.has("step")) {
        thermostat.step = obj["step"].asDouble();
    }
    if (!(thermostat.step > 0.0f) || (thermostat.step > 10.0f)) {
        thermostat.step = 0.5f;
    }
    if (thermostat.max_value <= thermostat.min_value) {
        thermostat.max_value = thermostat.min_value + thermostat.step;
    }

    const char* history = cstr(obj["history"]);
    if (history && strlen(history)) {
        if (strchr(history, '#') || strchr(history, '+')) {
            error = "\"history\" must be a plain topic";
            return false;
        }
        thermostat.history = out.strings.add(history);
    }

    out.thermostats.push_back(thermostat);
    item.thermostat = out.thermostats.size() - 1;
    item.target.entity = thermostat.setpoint;
    item.target.valid = true;
    return true;
}

// a printf format with exactly one float conversion (%f %e %g, flags/width/precision) and %%
bool validFormat(const espurna::string& format) {
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

bool parseItem(espurna::json::Value item_obj, Draft& out, DraftItem& item, std::vector<PendingOpen>& pending,
        const espurna::string& where, espurna::string& error, espurna::string& warnings)
{
    const espurna::string name = sanitize(cstr(item_obj["name"]), build::NameMax);
    item.name = out.strings.add(name);

    const char* item_type = cstr(item_obj["type"]);
    const bool open = item_obj.has("open") || item_obj.has("page");

    // "action" other than toggle / on / off: the action of another module (nspanelAction(), nspanel.h, e.g.
    // "matter-pair" of nspanelqr.cpp), a button of its own. Registered or not is known only at the tap: the module may be
    // installed later
    const char* action_name = cstr(item_obj["action"]);
    Action plain_action;
    const bool module_action = action_name && strlen(action_name) && !parseAction(action_name, plain_action);
    if (module_action) {
        bool valid = (strlen(action_name) <= ActionNameMax);
        for (const char* p = action_name; valid && *p; ++p) {
            valid = ((*p >= 'a') && (*p <= 'z')) || ((*p >= '0') && (*p <= '9')) || (*p == '-') || (*p == '_')
                || (*p == '.');
        }
        if (!valid) {
            error = where + " '" + name + "': unknown action '" + action_name + '\'';
            return false;
        }
        if (open || (item_type && strcmp(item_type, "button"))) {
            error = where + " '" + name + "': the action '" + action_name + "' goes on a button";
            return false;
        }
        item.action = out.strings.add(action_name);
    }

    uint16_t fallback = icons::LightSwitch;
    if (module_action) {
        item.type = ItemType::Button;
        fallback = icons::GestureTapButton;
    } else if (open && (!item_type || !strcmp(item_type, "link"))) {
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
        error = where + ": unknown item type '" + item_type + '\'';
        return false;
    }

    if (item.type == ItemType::Thermostat) {
        if (!parseThermostat(item_obj, out, item, error)) {
            error = where + " '" + name + "': " + error;
            return false;
        }
    } else if (!parseTarget(item_obj, out, item.target, error, module_action)) {
        error = where + " '" + name + "': " + error;
        return false;
    }

    // "page": a nested page, target.page is set once it is parsed (parsePage())
    if ((item.type == ItemType::Link) && item_obj.has("page")) {
        if (!item_obj["page"].isObject()) {
            error = where + " '" + name + "': \"page\" must be an object";
            return false;
        }
    } else if (item.type == ItemType::Link) {
        const auto& value = item_obj["open"];
        if (value.isNumber()) {
            item.target.page = static_cast<int>(value.asLong());
            if ((item.target.page < 0) && (item.target.page != PageNone)) {
                item.target.page = PageNone;
            }
        } else {
            const char* ref = cstr(value);
            if (ref && !strcmp(ref, "home")) {
                item.target.page = PageHome;
            } else if (ref && !strcmp(ref, "weather")) {
                item.target.page = PageWeather;
            } else if (ref && !strcmp(ref, "settings")) {
                item.target.page = PageSettings;
            } else {
                pending.push_back(PendingOpen{out.items.size(), espurna::string(ref ? ref : "")});
            }
        }
    } else if ((item.type != ItemType::Thermostat) && !module_action) {
        const bool need_group = (item.type == ItemType::Scene);
        if (need_group ? (item.target.group < 0) : (item.target.entity < 0)) {
            error = where + " '" + name
                + (need_group ? "': scene needs \"group\"" : "': needs \"topic\", \"relay\" or \"url\"");
            return false;
        }
    }

    const char* history = cstr(item_obj["history"]);
    if ((item.type != ItemType::Thermostat) && history && strlen(history)) {
        if (strchr(history, '#') || strchr(history, '+')) {
            error = where + " '" + name + "': \"history\" must be a plain topic";
            return false;
        }
        item.history = out.strings.add(history);
    }

    item.icon = out.strings.add(resolveIcon(cstr(item_obj["icon"]), fallback, warnings));

    if (item_obj.has("color") && !parseColor(item_obj["color"], item.color)) {
        warnings += where + " '" + name + "': bad color ignored; ";
    }

    // same names as AppDaemon's grid 'font' option (pages.py), or the TFT font id 0..9
    if (item_obj.has("font")) {
        const auto& font = item_obj["font"];
        const char* font_name = cstr(font);
        if (font.isNumber() && (static_cast<int>(font.asLong()) >= 0) && (static_cast<int>(font.asLong()) <= 9)) {
            item.font = static_cast<int>(font.asLong());
        } else if (font_name && !strcmp(font_name, "small")) {
            item.font = 0;
        } else if (font_name && !strcmp(font_name, "medium-icon")) {
            item.font = 1;
        } else if (font_name && !strcmp(font_name, "medium")) {
            item.font = 2;
        } else if (font_name && !strcmp(font_name, "large")) {
            item.font = 3;
        } else {
            warnings += "item '";
            warnings += name;
            warnings += "': unknown font ignored; ";
        }
    }

    return true;
}

// an item's "page", parsed after all top-level pages (their indices stay those of "pages")
struct PendingPage {
    espurna::json::Value obj;
    size_t item;
};

bool parsePage(espurna::json::Value page_obj, Draft& out, std::vector<PendingOpen>& pending,
        std::vector<PendingPage>& nested, size_t page_index, const espurna::string& fallback_title,
        espurna::string& error, espurna::string& warnings)
{
    const char* type = cstr(page_obj["type"]);
    DraftPage page;
    if (type && !strcmp(type, "settings")) {
        page.settings = true;
    } else if (type && !strcmp(type, "list")) {
        page.list = true;
    } else if (type && strcmp(type, "grid")) {
        warnings += "page type '";
        warnings += type;
        warnings += "' shown as grid; ";
    }

    const char* id = cstr(page_obj["id"]);
    espurna::string page_id(id ? id : "");
    if (!page_id.length()) {
        page_id = espurna::string('p');
        page_id += espurna::string(static_cast<unsigned>(page_index));
    }
    page.id = out.strings.add(page_id);

    espurna::string title = sanitize(cstr(page_obj["title"]), build::HeadingMax);
    if (!title.length()) {
        title = sanitize(fallback_title.c_str(), build::HeadingMax);
    }
    page.title = out.strings.add(title);

    if (page_obj.has("columns")) {
        const int columns = static_cast<int>(page_obj["columns"].asLong());
        if ((columns == 3) || (columns == 4)) {
            page.grid = (columns == 3) ? Grid::G3x2 : Grid::G4x2;
        } else {
            warnings += "page ";
            warnings += page_index;
            warnings += ": columns must be 3 or 4; ";
        }
    }

    // "layout" wins over "columns"
    if (page_obj.has("layout")) {
        const char* layout = cstr(page_obj["layout"]);
        bool found = false;
        for (size_t index = 0; index < (sizeof(GridNames) / sizeof(GridNames[0])); ++index) {
            if (layout && !strcmp(layout, GridNames[index])) {
                page.grid = static_cast<Grid>(index);
                found = true;
            }
        }
        if (!found) {
            warnings += "page ";
            warnings += page_index;
            warnings += ": layout must be 4x2, 3x3, 3x2, 2x2 or 1x2; ";
        }
    }

    if (page_obj.has("per_screen")) {
        const int per_screen = static_cast<int>(page_obj["per_screen"].asLong());
        if ((per_screen >= 1) && (per_screen <= static_cast<int>(TilesMax))) {
            page.per_screen = per_screen;
        } else {
            warnings += "page ";
            warnings += page_index;
            warnings += ": per_screen must be 1..9; ";
        }
    }

    if (page_obj.has("draw")) {
        page.draw = page_obj["draw"].asBool();
    }

    const auto items = page_obj["items"];
    if (page.settings && items.valid() && items.size()) {
        warnings += "settings page: items ignored; ";
    } else if (items.valid()) {
        page.items.reserve(items.size());
        const espurna::string where = espurna::string("page ") + page_index + " item";
        for (auto item_variant : items) {
            const auto item_obj = item_variant;
            if (!item_obj.valid()) {
                error = "item is not an object";
                return false;
            }

            DraftItem item;
            if (!parseItem(item_obj, out, item, pending, where, error, warnings)) {
                return false;
            }

            out.items.push_back(item);
            page.items.push_back(out.items.size() - 1);

            const auto sub = item_obj["page"];
            if (sub.valid() && (out.items.back().type == ItemType::Link)) {
                nested.push_back(PendingPage{sub, out.items.size() - 1});
            } else if (sub.valid()) {
                warnings += where + " '" + out.strings.at(out.items.back().name)
                    + "': \"page\" needs no \"type\", ignored; ";
            }
        }
    }

    out.pages.push_back(std::move(page));
    return true;
}

bool parseRoot(espurna::json::Value root, Draft& out, espurna::string& error, espurna::string& warnings) {
    const auto mqtt = root["mqtt"];
    if (mqtt.valid()) {
        if (mqtt.has("on")) out.on = out.strings.add(cstr(mqtt["on"]));
        if (mqtt.has("off")) out.off = out.strings.add(cstr(mqtt["off"]));
        if (mqtt.has("toggle")) out.toggle = out.strings.add(cstr(mqtt["toggle"]));
        if (mqtt.has("set")) out.set = out.strings.add(cstr(mqtt["set"]));
    }

    const auto screensaver = root["screensaver"];
    if (screensaver.valid()) {
        if (screensaver.has("timeout")) {
            out.timeout = clampValue<uint16_t>(screensaver["timeout"].asLong(), 0, 3600);
        }
        if (screensaver.has("brightness")) {
            out.brightness = clampValue<uint8_t>(screensaver["brightness"].asLong(), 1, 100);
        }
        if (screensaver.has("dim")) {
            out.dim = clampValue<uint8_t>(screensaver["dim"].asLong(), 0, 100);
        }

        const auto weather = screensaver["weather"];
        if (weather.valid() && weather.has("lat") && weather.has("lon")) {
            out.weather = weather["enabled"].asBool(true);
            out.lat = weather["lat"].asDouble();
            out.lon = weather["lon"].asDouble();
            if (weather.has("refresh")) {
                out.refresh = clampValue<uint32_t>(weather["refresh"].asLong(), 60, 86400);
            }
        }
    }

    // TFT sleep timer only runs when dim != brightness
    if (out.dim == out.brightness) {
        out.dim = out.brightness - 1;
    }

    const auto temperature = root["temperature"];
    if (temperature.valid()) {
        out.temperature = temperature["enabled"].asBool(true);
        out.temperature_show = temperature["show"].asBool(true);
        // the NTC is an ESPurna sensor: its correction and MQTT interval are the sensor settings
        if (temperature.has("offset") || temperature.has("publish")) {
            warnings += "temperature: offset and publish are ignored, use the Sensors settings of the web UI; ";
        }
    }

    const auto colors = root["colors"];
    if (colors.valid()) {
        if (colors.has("on")) out.color_on = static_cast<unsigned int>(colors["on"].asULong());
        if (colors.has("off")) out.color_off = static_cast<unsigned int>(colors["off"].asULong());
        if (colors.has("unknown")) out.color_unknown = static_cast<unsigned int>(colors["unknown"].asULong());
    }

    out.beep = root["beep"].asBool(false);
    out.debug = root["debug"].asBool(false);

    if (root.has("display")) {
        const char* display = cstr(root["display"]);
        if (display && !strcmp(display, "ohpanel")) {
            out.display = DriverPref::Ohpanel;
        } else if (display && !strcmp(display, "lovelace")) {
            out.display = DriverPref::Lovelace;
        } else if (!display || strcmp(display, "auto")) {
            warnings += "display: auto, ohpanel or lovelace (using auto); ";
        }
    }

    if (root.has("language")) {
        const char* language = cstr(root["language"]);
        if (language && !strcmp(language, "ru")) {
            out.language = lang::Language::Russian;
        } else if (!language || strcmp(language, "en")) {
            warnings += "language: en or ru (using en); ";
        }
    }

    const auto ohpanel = root["ohpanel"];
    if (ohpanel.valid()) {
        if (ohpanel.has("background")) {
            const char* name = cstr(ohpanel["background"]);
            bool found = false;
            for (size_t index = 0; index < (sizeof(BackgroundNames) / sizeof(BackgroundNames[0])); ++index) {
                if (name && !strcmp(name, BackgroundNames[index])) {
                    out.background = index;
                    found = true;
                }
            }
            if (!found) {
                warnings += "ohpanel.background: aurora, black, candy, dunes or forest; ";
            }
        }

        if (ohpanel.has("accent") && !parseColor(ohpanel["accent"], out.accent)) {
            warnings += "ohpanel.accent: bad colour; ";
        }
        if (ohpanel.has("text") && !parseColor(ohpanel["text"], out.text_color)) {
            warnings += "ohpanel.text: bad colour; ";
        }
        if (ohpanel.has("dim") && !parseColor(ohpanel["dim"], out.dim_color)) {
            warnings += "ohpanel.dim: bad colour; ";
        }
        if (ohpanel.has("weather_days")) {
            out.weather_days = clampValue<uint8_t>(ohpanel["weather_days"].asLong(), 0, 7);
        }

        if (ohpanel.has("layout")) {
            const char* layout = cstr(ohpanel["layout"]);
            if (layout && !strcmp(layout, "corner")) {
                out.layout = Layout::Corner;
            } else if (layout && !strcmp(layout, "center")) {
                out.layout = Layout::Center;
            } else {
                warnings += "ohpanel.layout: center or corner; ";
            }
        }

        if (ohpanel.has("theme")) {
            const char* theme = cstr(ohpanel["theme"]);
            if (theme && !strcmp(theme, "light")) {
                out.theme = Theme::Light;
            } else if (theme && !strcmp(theme, "dark")) {
                out.theme = Theme::Dark;
            } else {
                warnings += "ohpanel.theme: dark or light; ";
            }
        }

        const auto relays = ohpanel["relays"];
        if (relays.valid()) {
            for (size_t index = 0; (index < 2) && (index < relays.size()); ++index) {
                out.relay_labels[index] = out.strings.add(sanitize(cstr(relays[index]), 26));
            }
        }
    }

    const auto groups = root["groups"];
    if (groups.valid()) {
        for (auto kv : groups) {
            const auto members = kv.value;
            if (!members.valid()) {
                error = "group '";
                error += cstr(kv.key);
                error += "' is not an array";
                return false;
            }

            DraftGroup group;
            group.members.reserve(members.size());
            for (auto member : members) {
                if (member.isString()) {
                    const char* topic = cstr(member);
                    if (!topic || !strlen(topic)) {
                        continue;
                    }
                    group.members.push_back(findOrAddTopic(out, out.strings.add(topic)));
                } else if (member.isNumber()) {
                    const int relay = static_cast<int>(member.asLong());
                    if ((relay >= 0) && (static_cast<size_t>(relay) < limits.relays)) {
                        group.members.push_back(findOrAddRelay(out, relay));
                        continue;
                    }
                    error = "no such relay in group '";
                    error += cstr(kv.key);
                    error += '\'';
                    return false;
                }
            }

            group.name = out.strings.add(cstr(kv.key));
            out.groups.push_back(std::move(group));
        }
    }

    const auto pages = root["pages"];
    if (!pages.valid()) {
        error = "'pages' must be an array";
        return false;
    }

    // the record lists are deques (no doubling, no big block); the few shortcuts are one small vector
    {
        const auto shortcuts = screensaver["shortcuts"];
        out.shortcuts.reserve(std::min<size_t>(7, shortcuts.size()));
    }

    std::vector<PendingOpen> pending;

    std::vector<PendingPage> nested;
    size_t page_index = 0;
    for (auto page_variant : pages) {
        const auto page_obj = page_variant;
        if (!page_obj.valid()) {
            error = "page is not an object";
            return false;
        }
        if (!parsePage(page_obj, out, pending, nested, page_index, espurna::string(), error, warnings)) {
            return false;
        }
        ++page_index;
    }

    // nested pages after the swipe row, breadth first (a sub-page may hold further sub-pages)
    for (size_t index = 0; index < nested.size(); ++index) {
        const auto job = nested[index];
        const espurna::string fallback(out.strings.at(out.items[job.item].name));
        if (!parsePage(job.obj, out, pending, nested, out.pages.size(), fallback, error, warnings)) {
            return false;
        }
        out.pages.back().nested = true;
        out.items[job.item].target.page = out.pages.size() - 1;
    }

    // home shortcuts (OHpanel, up to 7): an item name, {"ref": "<item name>"} or a full item
    const auto shortcuts = screensaver["shortcuts"];
    if (shortcuts.valid()) {
        for (auto shortcut : shortcuts) {
            if (out.shortcuts.size() >= 7) {
                warnings += "more than 7 shortcuts, rest ignored; ";
                break;
            }

            const auto obj = shortcut;
            const char* ref = shortcut.isString() ? cstr(shortcut)
                : (obj.valid() ? cstr(obj["ref"]) : nullptr);
            if (ref) {
                bool found = false;
                for (size_t index = 0; index < out.items.size(); ++index) {
                    if (!strcmp(out.strings.at(out.items[index].name), ref)) {
                        out.shortcuts.push_back(index);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    warnings += "shortcut '";
                    warnings += ref;
                    warnings += "': no such item; ";
                }
                continue;
            }

            if (!obj.valid()) {
                error = "shortcut is not an object or an item name";
                return false;
            }

            DraftItem item;
            if (!parseItem(obj, out, item, pending, espurna::string("shortcut"), error, warnings)) {
                return false;
            }
            out.items.push_back(item);
            out.shortcuts.push_back(out.items.size() - 1);
        }
    }

    // home info lines (OHpanel info1 / info2): icon, text, a value from a topic (+ an extra value)
    const auto info = screensaver["info"];
    if (info.valid()) {
        for (auto line_variant : info) {
            if (out.info_count >= InfoLinesMax) {
                warnings += "more than 2 info lines, rest ignored; ";
                break;
            }

            const auto obj = line_variant;
            if (!obj.valid()) {
                error = "info line is not an object";
                return false;
            }

            auto& line = out.info[out.info_count];
            const espurna::string where = espurna::string("info ") + (out.info_count + 1);
            const char* icon = cstr(obj["icon"]);
            line.icon = (icon && strlen(icon)) ? out.strings.add(resolveIcon(icon, 0, warnings)) : 0;
            line.text = out.strings.add(sanitize(cstr(obj["text"]), 31));

            espurna::string format;
            espurna::string extra_format;
            const char* topic = cstr(obj["topic"]);
            if (topic && strlen(topic)) {
                if (strchr(topic, '#') || strchr(topic, '+')) {
                    error = where + ": invalid topic";
                    return false;
                }

                DraftEntity entity;
                entity.topic = out.strings.add(topic);
                if (obj.has("path") && !parsePath(out, obj["path"], entity.path)) {
                    error = where + ": bad \"path\"";
                    return false;
                }
                line.entity = findOrAddEntity(out, entity);
                format = jsonText(obj["format"]);

                const auto extra = obj["extra"];
                if (extra.valid()) {
                    DraftEntity second;
                    const char* extra_topic = cstr(extra["topic"]);
                    second.topic = out.strings.add((extra_topic && strlen(extra_topic)) ? extra_topic : topic);
                    if (extra.has("path") && !parsePath(out, extra["path"], second.path)) {
                        error = where + ": bad extra \"path\"";
                        return false;
                    }
                    line.extra = findOrAddEntity(out, second);
                    extra_format = jsonText(extra["format"]);
                }
            }

            if (format.length() && !validFormat(format)) {
                warnings += where + ": format needs one %f / %g conversion, ignored; ";
                format = espurna::string();
            }
            if (extra_format.length() && !validFormat(extra_format)) {
                warnings += where + ": extra format needs one %f / %g conversion, ignored; ";
                extra_format = espurna::string();
            }
            line.format = out.strings.add(format);
            line.extra_format = out.strings.add(extra_format);

            ++out.info_count;
        }
    }

    for (const auto& open : pending) {
        auto& target = out.items[open.item].target;
        for (size_t index = 0; index < out.pages.size(); ++index) {
            if ((open.ref == out.strings.at(out.pages[index].id)) || (open.ref == out.strings.at(out.pages[index].title))) {
                target.page = index;
                break;
            }
        }
        if (target.page == PageNone) {
            error = espurna::string("item '") + out.strings.at(out.items[open.item].name) + "': unknown page '"
                + open.ref + '\'';
            return false;
        }
    }

    for (auto& item : out.items) {
        if ((item.type == ItemType::Link) && (item.target.page >= static_cast<int>(out.pages.size()))) {
            error = espurna::string("item '") + out.strings.at(item.name) + "': no page " + item.target.page;
            return false;
        }
        // an "open" tile without "name" shows the title of its page
        if ((item.type == ItemType::Link) && (item.target.page >= 0) && !item.name) {
            item.name = out.strings.add(sanitize(out.strings.at(out.pages[item.target.page].title), build::NameMax));
        }
    }
    const auto buttons = root["buttons"];
    if (buttons.valid()) {
        out.buttons.reserve(buttons.size());
        for (auto button_variant : buttons) {
            Target target;
            const auto button_obj = button_variant;
            if (button_obj.valid() && !parseTarget(button_obj, out, target, error)) {
                error = espurna::string("button ") + out.buttons.size() + ": " + error;
                return false;
            }
            out.buttons.push_back(target);
        }
    }

    // mqtt.cpp drops MQTT_MESSAGE_EVENT payloads of MQTT_BUFFER_MAX_SIZE bytes and more
    // (Istabai istabai/out with 9 rooms is ~1.3 KB)
    if (limits.mqtt_buffer && !out.thermostats.empty() && (limits.mqtt_buffer < 2048)) {
        warnings += "mqttBuffer is ";
        warnings += espurna::string(static_cast<unsigned>(limits.mqtt_buffer));
        warnings += ": longer state payloads are dropped by the MQTT module (set mqttBuffer 2048); ";
    }

    return true;
}

// -----------------------------------------------------------------------------
// The tree: the accepted JSON, unknown keys dropped, in the order given. u16 little endian, byte aligned:
//   header   <strings: offset of the string table in the image> <root: node of the root object>
//   value    <tag> <u16>: 'o' object / 'a' array and the node offset (from the start of the tree), 's' string /
//            'r' number, true, false or null as written, and the offset of the text in the string table
//   object   <count> { <key text> <value> }
//   array    <count> { <value> }
// -----------------------------------------------------------------------------

enum class Kind : uint8_t {
    Any,        // everything kept as it is
    Root,
    Mqtt,
    Screensaver,
    Weather,
    Temperature,
    Colors,
    Ohpanel,
    Groups,     // any key (the group names), arrays of members
    Page,
    Item,
    Shortcut,
    Info,
    Extra,
    Button,
};

struct KeyKind {
    const char* key;
    Kind kind;
};

constexpr KeyKind RootKeys[] {
    {"mqtt", Kind::Mqtt}, {"screensaver", Kind::Screensaver}, {"temperature", Kind::Temperature},
    {"colors", Kind::Colors}, {"beep", Kind::Any}, {"debug", Kind::Any}, {"display", Kind::Any},
    {"language", Kind::Any}, {"ohpanel", Kind::Ohpanel}, {"groups", Kind::Groups}, {"pages", Kind::Page},
    {"buttons", Kind::Button},
};

constexpr KeyKind MqttKeys[] {
    {"on", Kind::Any}, {"off", Kind::Any}, {"toggle", Kind::Any}, {"set", Kind::Any},
};

constexpr KeyKind ScreensaverKeys[] {
    {"timeout", Kind::Any}, {"brightness", Kind::Any}, {"dim", Kind::Any}, {"weather", Kind::Weather},
    {"shortcuts", Kind::Shortcut}, {"info", Kind::Info},
};

constexpr KeyKind WeatherKeys[] {
    {"enabled", Kind::Any}, {"lat", Kind::Any}, {"lon", Kind::Any}, {"refresh", Kind::Any},
};

constexpr KeyKind TemperatureKeys[] {
    {"enabled", Kind::Any}, {"show", Kind::Any},
};

constexpr KeyKind ColorsKeys[] {
    {"on", Kind::Any}, {"off", Kind::Any}, {"unknown", Kind::Any},
};

constexpr KeyKind OhpanelKeys[] {
    {"background", Kind::Any}, {"accent", Kind::Any}, {"text", Kind::Any}, {"dim", Kind::Any},
    {"weather_days", Kind::Any}, {"layout", Kind::Any}, {"theme", Kind::Any}, {"relays", Kind::Any},
};

constexpr KeyKind PageKeys[] {
    {"type", Kind::Any}, {"id", Kind::Any}, {"title", Kind::Any}, {"columns", Kind::Any}, {"layout", Kind::Any},
    {"per_screen", Kind::Any}, {"draw", Kind::Any}, {"items", Kind::Item},
};

// what parseItem(), parseTarget() and parseThermostat() read
constexpr KeyKind ItemKeys[] {
    {"name", Kind::Any}, {"type", Kind::Any}, {"open", Kind::Any}, {"page", Kind::Page}, {"icon", Kind::Any},
    {"color", Kind::Any}, {"font", Kind::Any}, {"history", Kind::Any}, {"action", Kind::Any}, {"url", Kind::Any},
    {"on_url", Kind::Any}, {"off_url", Kind::Any}, {"relay", Kind::Any}, {"topic", Kind::Any},
    {"command", Kind::Any}, {"path", Kind::Any}, {"on", Kind::Any}, {"off", Kind::Any}, {"payload", Kind::Any},
    {"group", Kind::Any}, {"room", Kind::Any}, {"current", Kind::Any}, {"humidity", Kind::Any},
    {"active", Kind::Any}, {"min", Kind::Any}, {"max", Kind::Any}, {"setpoint", Kind::Any}, {"step", Kind::Any},
    {"ref", Kind::Any}, // only for Kind::Shortcut, ref. known()
};

constexpr KeyKind InfoKeys[] {
    {"icon", Kind::Any}, {"text", Kind::Any}, {"topic", Kind::Any}, {"path", Kind::Any}, {"format", Kind::Any},
    {"extra", Kind::Extra},
};

constexpr KeyKind ExtraKeys[] {
    {"topic", Kind::Any}, {"path", Kind::Any}, {"format", Kind::Any},
};

constexpr KeyKind ButtonKeys[] {
    {"action", Kind::Any}, {"url", Kind::Any}, {"on_url", Kind::Any}, {"off_url", Kind::Any},
    {"relay", Kind::Any}, {"topic", Kind::Any}, {"command", Kind::Any}, {"path", Kind::Any}, {"on", Kind::Any},
    {"off", Kind::Any}, {"payload", Kind::Any}, {"group", Kind::Any}, {"page", Kind::Any},
};

template <size_t Size>
bool find(const KeyKind (&keys)[Size], const char* key, size_t count, Kind& out) {
    for (size_t index = 0; index < std::min(Size, count); ++index) {
        if (!strcmp(keys[index].key, key)) {
            out = keys[index].kind;
            return true;
        }
    }
    return false;
}

// a key of an object of this kind: false - not one the parser reads (dropped)
bool known(Kind kind, const char* key, Kind& out) {
    constexpr size_t All { 64 };
    switch (kind) {
    case Kind::Any:
    case Kind::Groups:
        out = Kind::Any;
        return true;
    case Kind::Root:
        return find(RootKeys, key, All, out);
    case Kind::Mqtt:
        return find(MqttKeys, key, All, out);
    case Kind::Screensaver:
        return find(ScreensaverKeys, key, All, out);
    case Kind::Weather:
        return find(WeatherKeys, key, All, out);
    case Kind::Temperature:
        return find(TemperatureKeys, key, All, out);
    case Kind::Colors:
        return find(ColorsKeys, key, All, out);
    case Kind::Ohpanel:
        return find(OhpanelKeys, key, All, out);
    case Kind::Page:
        return find(PageKeys, key, All, out);
    case Kind::Item:
        return find(ItemKeys, key, (sizeof(ItemKeys) / sizeof(ItemKeys[0])) - 1, out);
    case Kind::Shortcut:
        return find(ItemKeys, key, All, out);
    case Kind::Info:
        return find(InfoKeys, key, All, out);
    case Kind::Extra:
        return find(ExtraKeys, key, All, out);
    case Kind::Button:
        return find(ButtonKeys, key, All, out);
    }

    return false;
}

const char* kindName(Kind kind) {
    switch (kind) {
    case Kind::Root:
        return "the config";
    case Kind::Mqtt:
        return "mqtt";
    case Kind::Screensaver:
        return "screensaver";
    case Kind::Weather:
        return "screensaver.weather";
    case Kind::Temperature:
        return "temperature";
    case Kind::Colors:
        return "colors";
    case Kind::Ohpanel:
        return "ohpanel";
    case Kind::Page:
        return "a page";
    case Kind::Item:
        return "an item";
    case Kind::Shortcut:
        return "a shortcut";
    case Kind::Info:
        return "an info line";
    case Kind::Extra:
        return "an info line's extra";
    case Kind::Button:
        return "a button";
    case Kind::Any:
    case Kind::Groups:
        break;
    }
    return "";
}

void put16(Bytes& out, uint16_t value) {
    out.push_back(value & 0xFF);
    out.push_back(value >> 8);
}

// a value of the tree: its tag and a node offset ('o', 'a') or a text ('s', 'r')
struct TreeValue {
    uint8_t tag;
    uint16_t at;
};

// the value of 'value' (an object or an array as a node, its children first); false - the tree is full or out of
// memory (draft.tree.failed, draft.strings.failed)
bool treeValue(Draft& draft, espurna::json::Value value, Kind kind, TreeValue& out, espurna::string& warnings, int depth) {
    auto& tree = draft.tree;

    if (value.isObject() && (depth < build::ConfigNestingLimit)) {
        const auto obj = value;
        std::vector<uint16_t> keys;
        std::vector<TreeValue> values;
        keys.reserve(obj.size());
        values.reserve(obj.size());
        for (auto kv : obj) {
            Kind child;
            if (!known(kind, cstr(kv.key), child)) {
                warnings += "unknown key '";
                warnings += cstr(kv.key);
                warnings += "' in ";
                warnings += kindName(kind);
                warnings += " dropped; ";
                continue;
            }
            TreeValue entry;
            if (!treeValue(draft, kv.value, child, entry, warnings, depth + 1)) {
                return false;
            }
            keys.push_back(draft.strings.add(cstr(kv.key)));
            values.push_back(entry);
        }

        out = TreeValue{'o', static_cast<uint16_t>(tree.size())};
        put16(tree, keys.size());
        for (size_t index = 0; index < keys.size(); ++index) {
            put16(tree, keys[index]);
            tree.push_back(values[index].tag);
            put16(tree, values[index].at);
        }
    } else if (value.isArray() && (depth < build::ConfigNestingLimit)) {
        const auto array = value;
        std::vector<TreeValue> values;
        values.reserve(array.size());
        for (auto element : array) {
            TreeValue entry;
            if (!treeValue(draft, element, kind, entry, warnings, depth + 1)) {
                return false;
            }
            values.push_back(entry);
        }

        out = TreeValue{'a', static_cast<uint16_t>(tree.size())};
        put16(tree, values.size());
        for (const auto& entry : values) {
            tree.push_back(entry.tag);
            put16(tree, entry.at);
        }
    } else {
        // a quoted string is a string, numbers and true / false stay their text, null is "null"
        const char* text = cstr(value);
        const bool string = value.isString() && text;
        out = TreeValue{static_cast<uint8_t>(string ? 's' : 'r'), draft.strings.add(text ? text : "null")};
    }

    return !tree.failed && !draft.strings.failed && (tree.size() <= build::ConfigMax);
}

// -----------------------------------------------------------------------------
// The image: the draft laid out
// -----------------------------------------------------------------------------

size_t align4(size_t value) {
    return (value + 3) & ~static_cast<size_t>(3);
}

// The image goes to the sink as it is laid out, in the order of its offsets: nothing of it is held whole. Zeros fill the
// 4 byte alignment between the lists
struct Emit {
    explicit Emit(ImageSink& to) :
        sink(to)
    {}

    ImageSink& sink;
    size_t pos { 0 };
    bool ok { true };

    void put(const void* data, size_t length) {
        if (ok && length) {
            ok = sink.write(static_cast<const uint8_t*>(data), length);
            pos += length;
        }
    }

    // up to 'offset'; false - already past it (a layout bug)
    void to(size_t offset) {
        static constexpr uint8_t Zeros[4] {};
        if (pos > offset) {
            ok = false;
        }
        while (ok && (pos < offset)) {
            put(Zeros, std::min<size_t>(sizeof(Zeros), offset - pos));
        }
    }
};

// one record of the image, made in RAM before it is written at 'offset': its Str / Span fields are relative to where
// they will be in the image
template <typename T>
struct Record {
    alignas(T) uint8_t raw[sizeof(T)];
    size_t offset;
    T* value;

    explicit Record(size_t offset) :
        offset(offset)
    {
        std::memset(raw, 0, sizeof(raw));
        value = new (raw) T;
    }

    T* operator->() {
        return value;
    }

    size_t at(const void* field) const {
        return offset + (static_cast<const uint8_t*>(field) - raw);
    }

    void str(Str& field, size_t strings, Off text) const {
        field.rel = static_cast<int16_t>(static_cast<long>(strings + text) - static_cast<long>(at(&field)));
    }

    template <typename U>
    void span(Span<U>& field, size_t to, size_t count) const {
        field.rel = count ? static_cast<int16_t>(static_cast<long>(to) - static_cast<long>(at(&field))) : 0;
        field.count = count;
    }

    void emit(Emit& out) const {
        out.to(offset);
        out.put(raw, sizeof(raw));
    }
};

// the draft laid out into 'sink': sizes and offsets first, then the bytes in order
ParseResult layout(Draft& draft, ImageSink& sink, espurna::string& error) {
    // distinct state topics, in the order of the entities
    std::vector<Off> topics;
    for (const auto& entity : draft.entities) {
        if (entity.topic && (std::find(topics.begin(), topics.end(), entity.topic) == topics.end())) {
            topics.push_back(entity.topic);
        }
    }

    size_t page_items = 0;
    for (const auto& page : draft.pages) {
        page_items += page.items.size();
    }

    size_t members = 0;
    for (const auto& group : draft.groups) {
        members += group.members.size();
    }

    // the keys of every distinct path, one after the other
    size_t path_keys = 0;
    std::vector<uint16_t> path_first;
    path_first.reserve(draft.paths.size());
    for (const auto& path : draft.paths) {
        path_first.push_back(path_keys);
        path_keys += path.size();
    }

    size_t size = align4(sizeof(Image));
    const auto place = [&](size_t bytes) {
        const size_t offset = size;
        size = align4(size + bytes);
        return offset;
    };

    const size_t entities_at = place(draft.entities.size() * sizeof(Entity));
    const size_t items_at = place(draft.items.size() * sizeof(Item));
    const size_t pages_at = place(draft.pages.size() * sizeof(Page));
    const size_t thermostats_at = place(draft.thermostats.size() * sizeof(Thermostat));
    const size_t groups_at = place(draft.groups.size() * sizeof(Group));
    const size_t topics_at = place(topics.size() * sizeof(Topic));
    const size_t info_at = place(draft.info_count * sizeof(InfoLine));
    const size_t buttons_at = place(draft.buttons.size() * sizeof(Target));
    const size_t shortcuts_at = place(draft.shortcuts.size() * sizeof(uint16_t));
    const size_t page_items_at = place(page_items * sizeof(uint16_t));
    const size_t members_at = place(members * sizeof(int16_t));
    const size_t path_keys_at = place(path_keys * sizeof(Str));
    const size_t strings_at = place(draft.strings.size());
    const size_t tree_at = place(draft.tree.size());

    if (draft.strings.overflow || (size > build::ConfigMax)) {
        error = "config too large for the flash data of the module (compiled ";
        error += espurna::string(static_cast<unsigned>(size));
        error += " bytes, at most ";
        error += espurna::string(static_cast<unsigned>(build::ConfigMax));
        error += ')';
        return ParseResult::Invalid;
    }

    if (!sink.begin(size, error)) {
        return error.startsWith("out of memory") ? ParseResult::LowMemory : ParseResult::WriteFailed;
    }

    Emit emit(sink);
    const size_t s = strings_at;

    Record<Image> head(0);
    auto* image = head.value;
    const auto& w = head;
    image->layout = ImageLayout;
    image->tree_version = TreeVersion;
    image->size = size;
    image->tree = tree_at;

    w.str(image->on, s, draft.on);
    w.str(image->off, s, draft.off);
    w.str(image->toggle, s, draft.toggle);
    w.str(image->set, s, draft.set);
    w.str(image->relay_labels[0], s, draft.relay_labels[0]);
    w.str(image->relay_labels[1], s, draft.relay_labels[1]);

    image->timeout = draft.timeout;
    image->brightness = draft.brightness;
    image->dim = draft.dim;
    image->weather = draft.weather;
    image->temperature = draft.temperature;
    image->temperature_show = draft.temperature_show;
    image->beep = draft.beep;
    image->debug = draft.debug;
    image->display = draft.display;
    image->language = draft.language;
    image->background = draft.background;
    image->weather_days = draft.weather_days;
    image->layout_default = draft.layout;
    image->theme = draft.theme;
    image->color_on = draft.color_on;
    image->color_off = draft.color_off;
    image->color_unknown = draft.color_unknown;
    image->lat = draft.lat;
    image->lon = draft.lon;
    image->refresh = draft.refresh;
    image->accent = draft.accent;
    image->text_color = draft.text_color;
    image->dim_color = draft.dim_color;

    w.span(image->entities, entities_at, draft.entities.size());
    w.span(image->items, items_at, draft.items.size());
    w.span(image->pages, pages_at, draft.pages.size());
    w.span(image->thermostats, thermostats_at, draft.thermostats.size());
    w.span(image->groups, groups_at, draft.groups.size());
    w.span(image->topics, topics_at, topics.size());
    w.span(image->info, info_at, draft.info_count);
    w.span(image->buttons, buttons_at, draft.buttons.size());
    w.span(image->shortcuts, shortcuts_at, draft.shortcuts.size());
    head.emit(emit);

    for (size_t index = 0; index < draft.entities.size(); ++index) {
        const auto& from = draft.entities[index];
        Record<Entity> record(entities_at + (index * sizeof(Entity)));
        const auto& w = record;
        auto* entity = record.value;
        w.str(entity->topic, s, from.topic);
        w.str(entity->command, s, from.command);
        if (from.path >= 0) {
            w.span(entity->path, path_keys_at + (path_first[from.path] * sizeof(Str)), draft.paths[from.path].size());
        } else {
            w.span(entity->path, 0, 0);
        }
        w.str(entity->on, s, from.on);
        w.str(entity->off, s, from.off);
        w.str(entity->payload, s, from.payload);
        w.str(entity->url_on, s, from.url_on);
        w.str(entity->url_off, s, from.url_off);
        entity->relay = from.relay;
        record.emit(emit);
    }

    for (size_t index = 0; index < draft.items.size(); ++index) {
        const auto& from = draft.items[index];
        Record<Item> record(items_at + (index * sizeof(Item)));
        const auto& w = record;
        auto* item = record.value;
        w.str(item->name, s, from.name);
        w.str(item->icon, s, from.icon);
        w.str(item->history, s, from.history);
        item->type = from.type;
        item->font = from.font;
        item->color = from.color;
        item->thermostat = from.thermostat;
        w.str(item->action, s, from.action);
        item->target = from.target;
        record.emit(emit);
    }

    size_t page_item = 0;
    for (size_t index = 0; index < draft.pages.size(); ++index) {
        const auto& from = draft.pages[index];
        Record<Page> record(pages_at + (index * sizeof(Page)));
        const auto& w = record;
        auto* page = record.value;
        w.str(page->id, s, from.id);
        w.str(page->title, s, from.title);
        page->settings = from.settings;
        page->list = from.list;
        page->grid = from.grid;
        page->per_screen = from.per_screen;
        page->draw = from.draw;
        page->nested = from.nested;

        const size_t at = page_items_at + (page_item * sizeof(uint16_t));
        w.span(page->items, at, from.items.size());
        page_item += from.items.size();
        record.emit(emit);
    }

    for (size_t index = 0; index < draft.thermostats.size(); ++index) {
        const auto& from = draft.thermostats[index];
        Record<Thermostat> record(thermostats_at + (index * sizeof(Thermostat)));
        const auto& w = record;
        auto* thermostat = record.value;
        thermostat->current = from.current;
        thermostat->setpoint = from.setpoint;
        thermostat->humidity = from.humidity;
        thermostat->active = from.active;
        thermostat->min = from.min;
        thermostat->max = from.max;
        thermostat->min_value = from.min_value;
        thermostat->max_value = from.max_value;
        thermostat->step = from.step;
        w.str(thermostat->history, s, from.history);
        thermostat->reserved = 0;
        record.emit(emit);
    }

    size_t member = 0;
    for (size_t index = 0; index < draft.groups.size(); ++index) {
        const auto& from = draft.groups[index];
        Record<Group> record(groups_at + (index * sizeof(Group)));
        const auto& w = record;
        auto* group = record.value;
        w.str(group->name, s, from.name);

        const size_t at = members_at + (member * sizeof(int16_t));
        w.span(group->members, at, from.members.size());
        member += from.members.size();
        record.emit(emit);
    }

    for (size_t index = 0; index < topics.size(); ++index) {
        Record<Topic> record(topics_at + (index * sizeof(Topic)));
        record.str(record->name, s, topics[index]);
        record->reserved = 0;
        record.emit(emit);
    }

    for (size_t index = 0; index < draft.info_count; ++index) {
        const auto& from = draft.info[index];
        Record<InfoLine> record(info_at + (index * sizeof(InfoLine)));
        const auto& w = record;
        auto* line = record.value;
        w.str(line->icon, s, from.icon);
        w.str(line->text, s, from.text);
        line->entity = from.entity;
        w.str(line->format, s, from.format);
        line->extra = from.extra;
        w.str(line->extra_format, s, from.extra_format);
        record.emit(emit);
    }

    for (size_t index = 0; index < draft.buttons.size(); ++index) {
        Record<Target> record(buttons_at + (index * sizeof(Target)));
        *record.value = draft.buttons[index];
        record.emit(emit);
    }

    emit.to(shortcuts_at);
    emit.put(draft.shortcuts.data(), draft.shortcuts.size() * sizeof(uint16_t));

    emit.to(page_items_at);
    for (const auto& page : draft.pages) {
        emit.put(page.items.data(), page.items.size() * sizeof(uint16_t));
    }

    emit.to(members_at);
    for (const auto& group : draft.groups) {
        emit.put(group.members.data(), group.members.size() * sizeof(int16_t));
    }

    for (size_t path = 0; path < draft.paths.size(); ++path) {
        for (size_t key = 0; key < draft.paths[path].size(); ++key) {
            Record<Str> record(path_keys_at + ((path_first[path] + key) * sizeof(Str)));
            record.str(*record.value, s, draft.paths[path][key]);
            record.emit(emit);
        }
    }

    emit.to(strings_at);
    for (const auto& block : draft.strings.blocks) {
        emit.put(block.data.get(), block.used);
    }

    // the tree header: where the strings are, the root
    draft.tree.set(0, strings_at & 0xFF);
    draft.tree.set(1, strings_at >> 8);
    draft.tree.set(2, draft.tree_root & 0xFF);
    draft.tree.set(3, draft.tree_root >> 8);
    emit.to(tree_at);
    for (size_t index = 0; index < draft.tree.blocks.size(); ++index) {
        const size_t done = index * build::CompileBlock;
        emit.put(draft.tree.blocks[index].get(), std::min(build::CompileBlock, draft.tree.size() - done));
    }

    emit.to(size);
    if (!emit.ok || !sink.end(error)) {
        if (!error.length()) {
            error = "cannot write the compiled config";
        }
        return ParseResult::WriteFailed;
    }

    return ParseResult::Ok;
}

// -----------------------------------------------------------------------------
// The tree written back as JSON
// -----------------------------------------------------------------------------

uint16_t get16(const uint8_t* at) {
    return at[0] | (at[1] << 8);
}

struct JsonOut {
    JsonWriter write;
    void* ctx;
    size_t length { 0 };

    void put(const char* data, size_t size) {
        if (size) {
            write(ctx, data, size);
            length += size;
        }
    }

    void put(const char* data) {
        put(data, strlen(data));
    }

    void indent(int depth) {
        static const char Spaces[] = "                ";
        put("\n", 1);
        for (int left = depth * 2; left > 0; left -= 16) {
            put(Spaces, std::min(left, 16));
        }
    }

    void string(const char* text) {
        put("\"", 1);
        const char* run = text;
        for (const char* p = text; *p; ++p) {
            const auto c = static_cast<uint8_t>(*p);
            const char* escape = nullptr;
            char buf[8];
            switch (c) {
            case '"':
                escape = "\\\"";
                break;
            case '\\':
                escape = "\\\\";
                break;
            case '\n':
                escape = "\\n";
                break;
            case '\r':
                escape = "\\r";
                break;
            case '\t':
                escape = "\\t";
                break;
            case '\b':
                escape = "\\b";
                break;
            case '\f':
                escape = "\\f";
                break;
            default:
                if (c < 0x20) {
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    escape = buf;
                }
                break;
            }
            if (escape) {
                put(run, p - run);
                put(escape);
                run = p + 1;
            }
        }
        put(run, strlen(run));
        put("\"", 1);
    }
};

// a value of the tree (its tag and u16 at 'value')
void writeValue(const uint8_t* tree, const char* strings, const uint8_t* value, int depth, JsonOut& out) {
    const uint8_t tag = value[0];
    const uint16_t at = get16(value + 1);
    switch (tag) {
    case 'o':
    case 'a': {
        const uint8_t* node = tree + at;
        const uint16_t count = get16(node);
        const bool object = (tag == 'o');
        if (!count) {
            out.put(object ? "{}" : "[]");
            return;
        }
        out.put(object ? "{" : "[", 1);
        const uint8_t* part = node + 2;
        for (uint16_t index = 0; index < count; ++index) {
            if (index) {
                out.put(",", 1);
            }
            out.indent(depth + 1);
            if (object) {
                out.string(strings + get16(part));
                out.put(": ", 2);
                part += 2;
            }
            writeValue(tree, strings, part, depth + 1, out);
            part += 3;
        }
        out.indent(depth);
        out.put(object ? "}" : "]", 1);
        return;
    }
    case 's':
        out.string(strings + at);
        return;
    case 'r':
        out.put(strings + at);
        return;
    }

    out.put("null");
}

} // namespace

// The heap the parse of the text takes: the JSON is read where it is (json.h), only the strings the parsers ask
// for are copied (cstr(), freed after the compile): never more than the text itself
size_t jsonBufferSize(const char*, size_t length) {
    return length + 64;
}

bool ImageBlock::begin(size_t length, espurna::string& error) {
    data.reset(new (std::nothrow) uint8_t[length]);
    size = data ? length : 0;
    filled = 0;
    if (!data) {
        error = "out of memory (image), retry";
        return false;
    }
    return true;
}

bool ImageBlock::write(const uint8_t* bytes, size_t length) {
    if (!data || ((filled + length) > size)) {
        return false;
    }
    std::memcpy(data.get() + filled, bytes, length);
    filled += length;
    return true;
}

bool ImageBlock::end(espurna::string& error) {
    if (!data || (filled != size)) {
        error = "the image is incomplete";
        return false;
    }
    return true;
}

// 'text' is NUL terminated and destroyed by the in-place parse; nothing reaches 'out' before the whole draft is made.
// No exception leaves here (-fexceptions build: std::deque / std::vector throw bad_alloc)
ParseResult compile(char* text, const Limits& with, ImageSink& out, espurna::string& error, espurna::string& warnings) {
    limits = with;

    ESPURNA_TRY {
        Draft draft;
        {
            CstrArena arena;
            const auto root = espurna::json::parse(espurna::StringView(text));
            if (!root.isObject()) {
                error = "invalid JSON";
                return ParseResult::Invalid;
            }

            const bool parsed = parseRoot(root, draft, error, warnings);
            if (draft.strings.failed) {
                error = "out of memory (draft), retry";
                return ParseResult::LowMemory;
            }
            if (!parsed) {
                return ParseResult::Invalid;
            }

            // the header of the tree (filled by layout()), then the nodes
            for (int index = 0; index < 4; ++index) {
                draft.tree.push_back(0);
            }
            TreeValue value;
            if (!treeValue(draft, root, Kind::Root, value, warnings, 0)) {
                if (draft.tree.failed || draft.strings.failed) {
                    error = "out of memory (draft), retry";
                    return ParseResult::LowMemory;
                }
                error = "config too large for the flash data of the module";
                return ParseResult::Invalid;
            }
            draft.tree_root = value.at;
        } // the copied strings are freed before the image is laid out

        return layout(draft, out, error);
    } ESPURNA_CATCH_BAD_ALLOC {
        error = "out of memory, retry";
    }

    return ParseResult::LowMemory;
}

bool bind(Config& out, const uint8_t* data, size_t size, std::unique_ptr<uint8_t[]>& owned) {
    out = Config{};
    if (!data || (size < sizeof(Image))) {
        return false;
    }

    const auto* image = reinterpret_cast<const Image*>(data);
    if ((image->layout != ImageLayout) || (image->tree_version != TreeVersion) || (image->size != size)
        || (image->tree >= size))
    {
        return false;
    }

    out.on = image->on.text();
    out.off = image->off.text();
    out.toggle = image->toggle.text();
    out.set = image->set.text();
    out.relay_labels[0] = image->relay_labels[0].text();
    out.relay_labels[1] = image->relay_labels[1].text();

    out.timeout = image->timeout;
    out.brightness = image->brightness;
    out.dim = image->dim;
    out.weather = image->weather;
    out.lat = image->lat;
    out.lon = image->lon;
    out.refresh = image->refresh;
    out.temperature = image->temperature;
    out.temperature_show = image->temperature_show;
    out.beep = image->beep;
    out.debug = image->debug;
    out.color_on = image->color_on;
    out.color_off = image->color_off;
    out.color_unknown = image->color_unknown;
    out.display = image->display;
    out.language = image->language;
    out.background = image->background;
    out.accent = image->accent;
    out.text_color = image->text_color;
    out.dim_color = image->dim_color;
    out.weather_days = image->weather_days;
    out.layout = image->layout_default;
    out.theme = image->theme;

    out.shortcuts = View<uint16_t>(image->shortcuts);
    out.info = View<InfoLine>(image->info);
    out.info_count = image->info.size();
    out.thermostats = View<Thermostat>(image->thermostats);
    out.entities = View<Entity>(image->entities);
    out.groups = View<Group>(image->groups);
    out.items = View<Item>(image->items);
    out.pages = View<Page>(image->pages);
    out.buttons = View<Target>(image->buttons);
    out.topics = View<Topic>(image->topics);

    out.live.resize(out.entities.size());
    out.image = image;
    out.owned = std::move(owned);
    return true;
}

bool treeReadable(const uint8_t* data, size_t size) {
    if (!data || (size < 8)) {
        return false;
    }

    const auto* image = reinterpret_cast<const Image*>(data);
    return (image->tree_version == TreeVersion) && (image->size == size) && ((image->tree + 4u) <= size);
}

size_t writeJson(const uint8_t* data, JsonWriter write, void* ctx) {
    JsonOut out { write, ctx };
    const auto* image = reinterpret_cast<const Image*>(data);
    const uint8_t* tree = data + image->tree;
    const char* strings = reinterpret_cast<const char*>(data) + get16(tree);
    const uint8_t root[3] { 'o', tree[2], tree[3] };
    writeValue(tree, strings, root, 0, out);
    out.put("\n", 1);
    return out.length;
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
