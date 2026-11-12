#include <Arduino.h>
#include <unity.h>

#include <cstring>
#include <vector>

// the parser is built here as it is in the firmware (HOST_MOCK: Arduino String + ArduinoJson only)
#include <espurna/nspanel_config.cpp>

using namespace espurna::nspanel;

namespace {

ParseResult parse(const char* json, const Limits& limits, Config& out, String& error, String& warnings) {
    std::vector<char> text(json, json + strlen(json) + 1);
    const size_t size = jsonBufferSize(text.data(), text.size() - 1);
    return parseInPlace(text.data(), size, limits, out, error, warnings);
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

// the built-in config of nspanel_storage.cpp
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
    TEST_ASSERT_EQUAL(3, config.groups[0].size());
    TEST_ASSERT_EQUAL_STRING("home/a/set", config.entities[0].command.c_str());
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
    TEST_ASSERT_EQUAL_STRING("Hall\x1Fset_temperature", setpoint.path.c_str());
    TEST_ASSERT_EQUAL_STRING("Hall\x1Ftemperature", config.entities[thermostat.current].path.c_str());

    // nested page: its tile opens it, its item is parsed after the swipe row
    TEST_ASSERT(config.items[3].type == ItemType::Link);
    TEST_ASSERT_EQUAL(1, config.items[3].target.page);
    TEST_ASSERT_EQUAL_STRING("B", config.items[5].name.c_str());
    TEST_ASSERT_EQUAL_STRING("state", config.entities[config.items[5].target.entity].path.c_str());

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
    TEST_ASSERT(contains(warnings, "MQTT_BUFFER_MAX_SIZE is 1024"));
}

void test_no_mqtt_warning_without_mqtt() {
    constexpr char Text[] =
        R"({"pages": [{"items": [{"type": "thermostat", "topic": "t", "room": "r"}]}]})";

    Config config;
    String error;
    String warnings;
    TEST_ASSERT(ParseResult::Ok == parse(Text, relays(0, 0), config, error, warnings));
    TEST_ASSERT(!contains(warnings, "MQTT_BUFFER_MAX_SIZE"));
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

void test_buffer_size() {
    // every node of the parse fits the computed size: the JSON parses, and only then "pages" is missing
    // (a too small buffer fails as "JSON nesting / size not supported")
    const char* texts[] {
        R"({"a": [1, 2, [3, {"b": 4}]], "c": {}, "d": [], "e": "x,y]}"})",
        R"({"a": [[[[]]]], "b": [{}, {}, {"c": [1, {"d": [2, 3]}]}]})",
        "{\"a\": [1, 2, 3] /* comment, [x] */ }",
    };

    for (const auto* text : texts) {
        Config config;
        String error;
        String warnings;
        TEST_ASSERT(ParseResult::Invalid == parse(text, relays(0), config, error, warnings));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("'pages' must be an array", error.c_str(), text);
    }
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
    RUN_TEST(test_no_mqtt_warning_without_mqtt);
    RUN_TEST(test_errors);
    RUN_TEST(test_warnings);
    RUN_TEST(test_buffer_size);
    RUN_TEST(test_icons);
    return UNITY_END();
}
