#include <Arduino.h>
#include <unity.h>

#include <cstring>
#include <vector>

// the parser is built here as it is in the firmware (HOST_MOCK: Arduino String + ArduinoJson only)
#include <espurna/screen_pages_config.cpp>

using namespace espurna::pages;

namespace {

// compiled into an image in the heap, bound to 'out' (as the firmware does on a chip without a data view)
ParseResult parse(const char* json, const Limits& limits, Config& out, String& error, String& warnings) {
    std::vector<char> text(json, json + strlen(json) + 1);
    ImageBlock image;
    const auto result = compile(text.data(), limits, image, error, warnings);
    if (result != ParseResult::Ok) {
        return result;
    }

    const auto* data = image.data.get();
    TEST_ASSERT(bind(out, data, image.size, image.data));
    return result;
}

void appendText(void* ctx, const char* data, size_t length) {
    static_cast<String*>(ctx)->concat(data, length);
}

// the JSON the image gives back
String json(const Config& config) {
    String out;
    writeJson(reinterpret_cast<const uint8_t*>(config.image), appendText, &out);
    return out;
}

Limits relays(size_t count, size_t mqtt_buffer = 0) {
    Limits out;
    out.relays = count;
    out.mqtt_buffer = mqtt_buffer;
    return out;
}

bool contains(const String& text, const char* part) {
    return text.indexOf(part) >= 0;
}

// the built-in config of screen_pages_storage.cpp
constexpr char DefaultConfig[] =
    "{\"pages\":[{\"title\":\"NSPanel\",\"items\":["
    "{\"name\":\"Relay 1\",\"icon\":\"light-switch\",\"relay\":0},"
    "{\"name\":\"Relay 2\",\"icon\":\"light-switch\",\"relay\":1}]}],"
    "\"buttons\":[{\"relay\":0},{\"relay\":1}]}";

void test_weekday() {
    TEST_ASSERT_EQUAL(3, weekday(2026, 11, 11));
    TEST_ASSERT_EQUAL(6, weekday(2000, 1, 1));
    TEST_ASSERT_EQUAL(4, weekday(2024, 2, 29));
    TEST_ASSERT_EQUAL(0, weekday(2026, 3, 1));
    TEST_ASSERT_EQUAL(4, weekday(2026, 12, 31));
    // the month indexes a table: out of range used to read past it
    TEST_ASSERT_EQUAL(-1, weekday(2026, 0, 1));
    TEST_ASSERT_EQUAL(-1, weekday(2026, 13, 1));
    TEST_ASSERT_EQUAL(-1, weekday(2026, -5, 1));
}

void test_text_helpers() {
    String text("Привет");      // 2 bytes per letter
    utf8Truncate(text, 5);
    TEST_ASSERT_EQUAL_STRING("Пр", text.c_str());

    TEST_ASSERT_EQUAL_STRING("При", utf8Prefix(String("Привет"), 3).c_str());
    TEST_ASSERT_EQUAL_STRING("a-b c", sanitize("a~b\r\nc", 30).c_str());
    TEST_ASSERT_EQUAL_STRING("", sanitize(nullptr, 30).c_str());
    TEST_ASSERT_EQUAL_STRING("\xEE\xA5\xBD", utf8(0xE97D).c_str());
    TEST_ASSERT_EQUAL_STRING("", utf8(0).c_str());
}

void test_default_config() {
    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Ok == parse(DefaultConfig, relays(2), config, error, warnings));
    TEST_ASSERT_EQUAL_STRING("", error.c_str());
    TEST_ASSERT_EQUAL_STRING("", warnings.c_str());

    TEST_ASSERT_EQUAL(1, config.pages.size());
    TEST_ASSERT_EQUAL_STRING("NSPanel", config.pages[0].title.c_str());
    TEST_ASSERT_EQUAL(2, config.items.size());
    TEST_ASSERT_EQUAL(2, config.entities.size());
    TEST_ASSERT_EQUAL(0, config.entities[0].relay);
    TEST_ASSERT_EQUAL(1, config.entities[1].relay);
    TEST_ASSERT_EQUAL_STRING("\xEE\xA5\xBD", config.items[0].icon.c_str());

    // the buttons share the entities of the tiles
    TEST_ASSERT_EQUAL(2, config.buttons.size());
    TEST_ASSERT(config.buttons[0].valid);
    TEST_ASSERT_EQUAL(0, config.buttons[0].entity);
    TEST_ASSERT_EQUAL(1, config.buttons[1].entity);
}

void test_no_such_relay() {
    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Invalid == parse(DefaultConfig, relays(0), config, error, warnings));
    TEST_ASSERT(contains(error, "no such relay"));
}

void test_full_config() {
    constexpr char Text[] = R"({
        "mqtt": {"on": "ON", "off": "OFF"},
        "groups": {"lights": ["home/a", "home/b", 0]},
        "pages": [
            {"id": "main", "title": "Main", "layout": "3x3", "items": [
                {"name": "A", "topic": "home/a"},
                {"name": "All", "type": "scene", "group": "lights", "action": "off"},
                {"name": "Heat", "type": "thermostat", "topic": "istabai/out", "room": "Hall",
                 "command": "istabai/in/Hall"},
                {"name": "More", "page": {"title": "Sub", "items": [
                    {"name": "B", "topic": "home/b", "path": ["state"]}]}},
                {"name": "Go", "open": "main"}
            ]}
        ],
        "screensaver": {
            "shortcuts": ["A", {"ref": "Nope"}],
            "info": [{"text": "Out", "topic": "weather/t", "format": "%.1f C"}]
        },
        "buttons": [{"group": "lights"}],
        "language": "ru",
        "display": "ohpanel"
    })";

    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Ok == parse(Text, relays(1, 1024), config, error, warnings));
    TEST_ASSERT_EQUAL_STRING("", error.c_str());

    TEST_ASSERT_EQUAL_STRING("ON", config.on.c_str());
    TEST_ASSERT_EQUAL_STRING("OFF", config.off.c_str());
    TEST_ASSERT(config.language == lang::Language::Russian);
    TEST_ASSERT(config.display == DriverPref::Ohpanel);

    // groups: two topics and a relay
    TEST_ASSERT_EQUAL(1, config.groups.size());
    TEST_ASSERT_EQUAL(3, config.groups[0].members.size());
    TEST_ASSERT_EQUAL_STRING("lights", config.groups[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("", config.entities[0].command.c_str());
    TEST_ASSERT_EQUAL_STRING("home/a/set", entityCommand(config, config.entities[0]).c_str());
    TEST_ASSERT_EQUAL(0, config.entities[2].relay);

    // the swipe row page, then the nested one
    TEST_ASSERT_EQUAL(2, config.pages.size());
    TEST_ASSERT(config.pages[0].grid == Grid::G3x3);
    TEST_ASSERT(!config.pages[0].nested);
    TEST_ASSERT(config.pages[1].nested);
    TEST_ASSERT_EQUAL_STRING("Sub", config.pages[1].title.c_str());

    TEST_ASSERT_EQUAL(6, config.items.size());

    // the tile shares the group's entity
    TEST_ASSERT_EQUAL(0, config.items[0].target.entity);

    TEST_ASSERT(config.items[1].type == ItemType::Scene);
    TEST_ASSERT_EQUAL(0, config.items[1].target.group);
    TEST_ASSERT(config.items[1].target.action == Action::Off);

    // thermostat: [room, key] paths, the setpoint carries the command
    TEST_ASSERT(config.items[2].type == ItemType::Thermostat);
    TEST_ASSERT_EQUAL(1, config.thermostats.size());
    const auto& thermostat = config.thermostats[0];
    const auto& setpoint = config.entities[thermostat.setpoint];
    TEST_ASSERT_EQUAL(thermostat.setpoint, config.items[2].target.entity);
    TEST_ASSERT_EQUAL_STRING("istabai/in/Hall", setpoint.command.c_str());
    TEST_ASSERT_EQUAL(2, setpoint.path.size());
    TEST_ASSERT_EQUAL_STRING("Hall", setpoint.path[0].c_str());
    TEST_ASSERT_EQUAL_STRING("set_temperature", setpoint.path[1].c_str());
    TEST_ASSERT_EQUAL_STRING("temperature", config.entities[thermostat.current].path[1].c_str());
    // the room is one text for all paths of the thermostat
    TEST_ASSERT_EQUAL_PTR(setpoint.path[0].c_str(), config.entities[thermostat.current].path[0].c_str());

    // nested page: its tile opens it, its item is parsed after the swipe row
    TEST_ASSERT(config.items[3].type == ItemType::Link);
    TEST_ASSERT_EQUAL(1, config.items[3].target.page);
    TEST_ASSERT_EQUAL_STRING("B", config.items[5].name.c_str());
    TEST_ASSERT_EQUAL(1, config.entities[config.items[5].target.entity].path.size());
    TEST_ASSERT_EQUAL_STRING("state", config.entities[config.items[5].target.entity].path[0].c_str());

    // "open" by page id
    TEST_ASSERT(config.items[4].type == ItemType::Link);
    TEST_ASSERT_EQUAL(0, config.items[4].target.page);

    // shortcut by name; an unknown one is a warning
    TEST_ASSERT_EQUAL(1, config.shortcuts.size());
    TEST_ASSERT_EQUAL(0, config.shortcuts[0]);
    TEST_ASSERT(contains(warnings, "shortcut 'Nope': no such item"));

    TEST_ASSERT_EQUAL(1, config.info_count);
    TEST_ASSERT_EQUAL_STRING("%.1f C", config.info[0].format.c_str());
    TEST_ASSERT_EQUAL_STRING("weather/t", config.entities[config.info[0].entity].topic.c_str());

    TEST_ASSERT_EQUAL(1, config.buttons.size());
    TEST_ASSERT_EQUAL(0, config.buttons[0].group);

    // thermostat state payloads are longer than a 1 KB MQTT buffer
    TEST_ASSERT(contains(warnings, "mqttBuffer is 1024"));

    // one subscription per state topic: home/a, home/b, istabai/out, weather/t
    TEST_ASSERT_EQUAL(4, config.topics.size());
    TEST_ASSERT_EQUAL(config.entities.size(), config.live.size());
}

// GET /nspanel.json is written back from the image: unknown keys dropped (a warning at the upload), the rest kept as
// given, and the text it gives compiles to the same text again
void test_round_trip() {
    constexpr char Text[] = R"({"version": 1, "mqtt": {"on": "ON"},
        "pages": [{"title": "Main \"1\"", "items": [
            {"name": "A", "topic": "home/a", "path": ["state", 0], "min": 1.50, "extra": true},
            {"name": "B", "page": {"title": "Sub", "items": [{"name": "C", "relay": 0}]}},
            {"name": "M", "icon": "plus", "action": "matter-pair"}]}],
        "screensaver": {"info": [{"text": "T", "topic": "t", "extra": {"path": "h", "format": "%.0f%%"}}]},
        "groups": {"g": ["home/a", 0]}, "beep": false, "debug": null})";

    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Ok == parse(Text, relays(1), config, error, warnings));
    TEST_ASSERT(contains(warnings, "unknown key 'version' in the config dropped"));
    TEST_ASSERT(contains(warnings, "unknown key 'extra' in an item dropped"));

    // an action of another module: a button of its own, no target
    TEST_ASSERT(config.items[2].type == ItemType::Button);
    TEST_ASSERT_EQUAL_STRING("matter-pair", config.items[2].action.c_str());
    TEST_ASSERT_EQUAL(-1, config.items[2].target.entity);
    TEST_ASSERT_EQUAL_STRING("", config.items[0].action.c_str());

    const String first = json(config);
    TEST_ASSERT(!contains(first, "version"));
    TEST_ASSERT(contains(first, "\"action\": \"matter-pair\""));
    TEST_ASSERT(contains(first, "\"min\": 1.50"));
    TEST_ASSERT(contains(first, "\"title\": \"Main \\\"1\\\"\""));
    TEST_ASSERT(contains(first, "\"debug\": null"));
    TEST_ASSERT(contains(first, "\n  \"mqtt\": {\n    \"on\": \"ON\"\n  },"));

    Config again;
    String warnings_again;
    TEST_ASSERT(ParseResult::Ok == parse(first.c_str(), relays(1), again, error, warnings_again));
    TEST_ASSERT(!contains(warnings_again, "unknown key"));
    TEST_ASSERT_EQUAL_STRING(first.c_str(), json(again).c_str());
    TEST_ASSERT_EQUAL(config.entities.size(), again.entities.size());
    TEST_ASSERT_EQUAL(config.items.size(), again.items.size());
    TEST_ASSERT_EQUAL_STRING("matter-pair", again.items[2].action.c_str());
    TEST_ASSERT(again.items[2].type == ItemType::Button);
}

void test_no_mqtt_warning_without_mqtt() {
    constexpr char Text[] =
        R"({"pages": [{"items": [{"type": "thermostat", "topic": "t", "room": "r"}]}]})";

    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Ok == parse(Text, relays(0, 0), config, error, warnings));
    TEST_ASSERT(!contains(warnings, "mqttBuffer"));
}

void test_errors() {
    const struct {
        const char* text;
        const char* error;
    } cases[] {
        {"{", "JSON"},
        {R"({"pages": 1})", "'pages' must be an array"},
        {R"({"pages": [{"items": [{"name": "x", "open": "nowhere"}]}]})", "unknown page 'nowhere'"},
        {R"({"pages": [{"items": [{"name": "x", "topic": "a/#"}]}]})", "invalid topic"},
        {R"({"pages": [{"items": [{"name": "x", "type": "scene", "group": "none"}]}]})", "unknown group 'none'"},
        {R"({"pages": [{"items": [{"name": "x", "type": "slider", "topic": "a"}]}]})", "unknown item type 'slider'"},
        {R"({"pages": [{"items": [{"name": "x", "topic": "a", "path": null}]}]})", "bad \"path\""},
        {R"({"pages": [{"items": [{"name": "x", "url": "ftp://host"}]}]})", "\"url\" must start with http://"},
        {R"({"pages": [{"items": [{"name": "x", "action": "Pair Now"}]}]})", "unknown action 'Pair Now'"},
        {R"({"pages": [{"items": [{"name": "x", "type": "switch", "action": "matter-pair"}]}]})", "goes on a button"},
        {R"({"pages": [{"items": [{"name": "x"}]}]})", "needs \"topic\", \"relay\" or \"url\""},
    };

    for (const auto& test : cases) {
        Config config;
        String error;
        String warnings;
        const auto result = parse(test.text, relays(2), config, error, warnings);
        TEST_ASSERT_MESSAGE(ParseResult::Invalid == result, test.text);
        TEST_ASSERT_MESSAGE(contains(error, test.error), error.c_str());
    }
}

void test_warnings() {
    constexpr char Text[] = R"({
        "pages": [{"layout": "5x5", "per_screen": 20, "items": [
            {"name": "x", "topic": "a", "icon": "no-such-icon", "color": "#12", "font": "huge"}]}],
        "display": "other",
        "ohpanel": {"background": "space", "theme": "grey"}
    })";

    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Ok == parse(Text, relays(0), config, error, warnings));
    TEST_ASSERT(contains(warnings, "layout must be"));
    TEST_ASSERT(contains(warnings, "per_screen must be 1..9"));
    TEST_ASSERT(contains(warnings, "unknown icon 'no-such-icon'"));
    TEST_ASSERT(contains(warnings, "bad color ignored"));
    TEST_ASSERT(contains(warnings, "unknown font ignored"));
    TEST_ASSERT(contains(warnings, "display: auto, ohpanel or lovelace"));
    TEST_ASSERT(contains(warnings, "ohpanel.background"));
    TEST_ASSERT(contains(warnings, "ohpanel.theme"));
    TEST_ASSERT(config.display == DriverPref::Auto);
    TEST_ASSERT(config.pages[0].grid == Grid::G4x2);
}

// a parse into one buffer of exactly the given size
struct ExactJsonBuffer : public ArduinoJson::Internals::StaticJsonBufferBase {
    ExactJsonBuffer(char* buffer, size_t capacity) :
        StaticJsonBufferBase(buffer, capacity)
    {}
};

void test_buffer_size() {
    // jsonBufferSize() (the heap estimate of a compile) holds every node of the parse; the compile itself (nodes in
    // blocks) gets as far as the missing "pages"
    const char* texts[] {
        R"({"a": [1, 2, [3, {"b": 4}]], "c": {}, "d": [], "e": "x,y]}"})",
        R"({"a": [[[[]]]], "b": [{}, {}, {"c": [1, {"d": [2, 3]}]}]})",
        "{\"a\": [1, 2, 3] /* comment, [x] */ }",
    };

    for (const auto* text : texts) {
        std::vector<char> copy(text, text + strlen(text) + 1);
        const size_t size = jsonBufferSize(copy.data(), copy.size() - 1);
        std::vector<char> nodes(size);
        ExactJsonBuffer buffer(nodes.data(), size);
        TEST_ASSERT_MESSAGE(buffer.parseObject(copy.data(), build::ConfigNestingLimit).success(), text);

        Config config;
        String error;
        String warnings;
        TEST_ASSERT(ParseResult::Invalid == parse(text, relays(0), config, error, warnings));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("'pages' must be an array", error.c_str(), text);
    }
}

// more strings and tree than one compile block, one text longer than a block: the image is the same read back
void test_large_config() {
    String long_title;
    for (size_t index = 0; index < (build::CompileBlock + 200); ++index) {
        long_title += static_cast<char>('a' + (index % 26));
    }

    String text = F("{\"pages\": [{\"title\": \"");
    text += long_title;
    text += F("\", \"items\": [");
    for (int index = 0; index < 120; ++index) {
        if (index) {
            text += ',';
        }
        text += F("{\"name\": \"n");
        text += index;
        text += F("\", \"topic\": \"home/sensor/");
        text += index;
        text += F("\"}");
    }
    text += F("]}]}");

    Config config;
    String error;
    String warnings;
    TEST_ASSERT_MESSAGE(ParseResult::Ok == parse(text.c_str(), relays(0), config, error, warnings), error.c_str());
    TEST_ASSERT_EQUAL(120, config.items.size());
    TEST_ASSERT_EQUAL_STRING("n0", config.items[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("n119", config.items[119].name.c_str());
    TEST_ASSERT_EQUAL_STRING("home/sensor/119", config.entities[config.items[119].target.entity].topic.c_str());

    const String first = json(config);
    TEST_ASSERT(contains(first, long_title.c_str()));
    TEST_ASSERT(contains(first, "\"topic\": \"home/sensor/119\""));

    Config again;
    TEST_ASSERT(ParseResult::Ok == parse(first.c_str(), relays(0), again, error, warnings));
    TEST_ASSERT_EQUAL_STRING(first.c_str(), json(again).c_str());
}

// the sink refuses the bytes (a flash write failed): WriteFailed, an error
struct FailingSink : public ImageSink {
    size_t limit { 64 };
    size_t written { 0 };

    bool begin(size_t, String&) override {
        return true;
    }

    bool write(const uint8_t*, size_t length) override {
        written += length;
        return written <= limit;
    }

    bool end(String&) override {
        return true;
    }
};

void test_sink_failure() {
    std::vector<char> text(DefaultConfig, DefaultConfig + strlen(DefaultConfig) + 1);
    FailingSink sink;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::WriteFailed == compile(text.data(), relays(2), sink, error, warnings));
    TEST_ASSERT(error.length() > 0);
}

void test_icons() {
    TEST_ASSERT(iconCount() > 0);
    TEST_ASSERT_NOT_NULL(iconName(0));
    TEST_ASSERT_NULL(iconName(iconCount()));
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_weekday);
    RUN_TEST(test_text_helpers);
    RUN_TEST(test_default_config);
    RUN_TEST(test_no_such_relay);
    RUN_TEST(test_full_config);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_no_mqtt_warning_without_mqtt);
    RUN_TEST(test_errors);
    RUN_TEST(test_warnings);
    RUN_TEST(test_buffer_size);
    RUN_TEST(test_large_config);
    RUN_TEST(test_sink_failure);
    RUN_TEST(test_icons);
    return UNITY_END();
}
