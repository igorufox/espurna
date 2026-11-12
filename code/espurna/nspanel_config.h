/*

NSPANEL MODULE - config model and its parser

The model of /nspanel.json (pages of tiles, groups, shortcuts, hardware buttons, entities) and the
in-place JSON parser that builds it. Depends on Arduino String and ArduinoJson only, so the host
tests build it as it is (code/test/unit/src/nspanel)

*/

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "nspanel_icons.h"

namespace espurna {
namespace nspanel {

namespace build {

constexpr size_t ConfigMax { 16384 };

// ArduinoJson 5 stops at 10 levels by default; each nested "page" adds 3 (item, page, items)
constexpr uint8_t ConfigNestingLimit { 24 };

// TFT text limits, bytes (Max Text Size of the 51 HMI)
constexpr size_t HeadingMax { 40 };
constexpr size_t NameMax { 30 };
constexpr size_t DateMax { 40 };
constexpr size_t ForecastMax { 10 };

} // namespace build

// -----------------------------------------------------------------------------
// Text helpers
// -----------------------------------------------------------------------------

String utf8(uint16_t code);

// cut at 'max' bytes without splitting a multi-byte sequence
void utf8Truncate(String& value, size_t max);

// first 'chars' UTF-8 characters
String utf8Prefix(const String& value, size_t chars);

// '~' separates fields of ESP->TFT commands
String sanitize(const char* value, size_t max);

// Sakamoto, 0 = Sunday; -1 for a month outside 1..12
int weekday(int y, int m, int d);

namespace lang {

enum class Language : uint8_t {
    English,
    Russian,
};

} // namespace lang

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
    Link,       // "open": shows a page (target.page: page index, PageHome, PageWeather or PageSettings)
    Thermostat, // config.thermostats[Item::thermostat]; target.entity = the setpoint
};

enum class DriverPref : uint8_t {
    Auto,
    Ohpanel,
    Lovelace,
};

const char* driverPrefName(DriverPref value);

// Target::page values besides a page index
constexpr int PageNone { -1 };
constexpr int PageHome { -2 };
constexpr int PageWeather { -3 };
constexpr int PageSettings { -4 };

// separates the keys of Entity::path (one allocation instead of a vector of Strings)
constexpr char PathSep { '\x1F' };

struct Entity {
    String topic;    // absolute MQTT state topic, empty for local relays
    String command;  // absolute MQTT command topic
    int relay { -1 };
    int state { -1 }; // -1 unknown, 0 off, 1 on
    String value;

    // JSON state payloads (zigbee2mqtt, Istabai): keys into the payload, PathSep separated.
    // Empty: the payload itself is the value. Several entities may share one topic.
    String path;
    String on;       // state values for this entity, empty: mqtt.on / mqtt.off
    String off;
    String payload;  // command template, "%s" is replaced by the value; empty: the plain value
    float number { NAN }; // value as a number, NAN when it is not one

    // "url" / "on_url" / "off_url": an HTTP GET instead of the MQTT command ("url": the same for on and off)
    String url_on;
    String url_off;
};

// "type": "thermostat": entity indices into Config::entities, -1 when not configured
struct Thermostat {
    int current { -1 };
    int setpoint { -1 };  // also carries the command topic and payload template
    int humidity { -1 };
    int active { -1 };
    int min { -1 };
    int max { -1 };
    float min_value { 5.0f };  // limits while min/max are unknown, or fixed numbers from the config
    float max_value { 30.0f };
    float step { 0.5f };
    String history;       // topic of a retained history series, shown as a chart (empty: none)
};

// home info lines (OHpanel info1_* / info2_*, screensaver.info)
struct InfoLine {
    String icon;
    String text;
    int entity { -1 };
    String format;      // printf with one float conversion, empty: the value text as is
    int extra { -1 };
    String extra_format;
};

constexpr size_t InfoLinesMax { 2 };

enum class Layout : uint8_t {
    Center,  // big centred icon (font 9), name below
    Corner,  // icon top left, state line, name
};

enum class Theme : uint8_t {
    Dark,
    Light,   // light (white) tiles on the background, ref. ohpanel::tileColors()
};

struct Target {
    int entity { -1 };
    int group { -1 };
    int page { -1 };
    Action action { Action::Toggle };
    bool valid { false };
};

// TFT >= 53: an icon text ending in '¬<digit>' switches the grid tile to that font
constexpr uint8_t NoFont { 0xFF };

struct Item {
    String name;
    String icon;
    ItemType type { ItemType::Switch };
    uint8_t font { NoFont };
    int32_t color { -1 }; // OHpanel: icon colour (RGB565) when on, -1 = theme accent
    int16_t thermostat { -1 };
    String history;       // not a thermostat: a tap opens the chart of this retained series instead of switching
    Target target;
};

constexpr size_t TilesMax { 9 };

// "layout" of a grid page: columns x rows of tiles. OHpanel shows cols x rows tiles: the custom design
// d3731fbb on its own pages (buttons33 / buttons32 / buttons22 / buttons12), the stock design drawn at
// runtime on `buttons`; an unknown TFT design and Lovelace use cols columns of the 4x2 tile slots (2 rows)
enum class Grid : uint8_t {
    G4x2,
    G3x3,
    G3x2,
    G2x2,
    G1x2,
};

constexpr const char* GridNames[] { "4x2", "3x3", "3x2", "2x2", "1x2" };

constexpr uint8_t gridCols(Grid grid) {
    return (grid == Grid::G4x2) ? 4
        : ((grid == Grid::G3x3) || (grid == Grid::G3x2)) ? 3
        : (grid == Grid::G2x2) ? 2 : 1;
}

constexpr uint8_t gridRows(Grid grid) {
    return (grid == Grid::G3x3) ? 3 : 2;
}

// tiles of one screen on a TFT that has the grid's own page
constexpr size_t gridTiles(Grid grid) {
    return static_cast<size_t>(gridCols(grid)) * gridRows(grid);
}

inline const char* gridName(Grid grid) {
    return GridNames[static_cast<size_t>(grid)];
}

// OHpanel backgrounds, same order as ohpanel::Backgrounds
constexpr const char* BackgroundNames[] { "aurora", "black", "candy", "dunes", "forest" };

struct Page {
    String id;
    String title;
    bool settings { false }; // "type": "settings": the OHpanel settings tiles instead of items
    bool list { false };     // "type": "list": OHpanel entities_rows (one row per item), others draw a grid
    Grid grid { Grid::G4x2 };// "layout": "4x2" | "3x3" | "3x2" | "2x2" | "1x2"; "columns": 3 = "3x2"
    uint8_t per_screen { 0 };// "per_screen": tiles (or list rows) per screen, 0 = as many as fit
    bool draw { false };     // "draw": true: OHpanel draws the tiles at runtime (fill / cirs / xstr) even for 4x2
    bool nested { false };   // an item's "page": a sub-page, not in the swipe row, opened by its tile
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

    bool beep { false };
    bool debug { false };

    uint16_t color_on { icons::colors::On };
    uint16_t color_off { icons::colors::Off };
    uint16_t color_unknown { icons::colors::Unknown };

    DriverPref display { DriverPref::Auto };
    lang::Language language { lang::Language::English };

    // OHpanel ("ohpanel" object)
    uint8_t background { 0 };     // index into ohpanel::Backgrounds
    int32_t accent { -1 };        // RGB565 overrides of the background's theme, -1 = theme
    int32_t text_color { -1 };
    int32_t dim_color { -1 };
    uint8_t weather_days { 7 };
    String relay_labels[2];
    std::vector<uint16_t> shortcuts; // item indices (home shortcuts, screensaver.shortcuts)
    Layout layout { Layout::Center }; // ohpanel.layout; the nspIcons setting wins
    Theme theme { Theme::Dark };      // ohpanel.theme; the nspTheme setting wins
    InfoLine info[InfoLinesMax];
    size_t info_count { 0 };
    std::vector<Thermostat> thermostats;

    // ~124 bytes each and their count is only known after de-duplication: a vector growing to
    // 128 entries needs one 16 KB block (plus the old 8 KB one while it moves), which is what
    // ran the heap out on upload. deque keeps them in 512 byte chunks, nothing ever moves
    std::deque<Entity> entities;
    std::vector<String> group_names;
    std::vector<std::vector<int>> groups;
    std::vector<Item> items;
    std::vector<Page> pages;
    std::vector<Target> buttons;
};

// -----------------------------------------------------------------------------
// Parser
// -----------------------------------------------------------------------------

// what the parse checks against the rest of the firmware
struct Limits {
    size_t relays { 0 };       // "relay" indices below this are valid
    size_t mqtt_buffer { 0 };  // MQTT_BUFFER_MAX_SIZE, 0 without MQTT
};

enum class ParseResult {
    Ok,
    Invalid,
    LowMemory,
};

// Exact node storage of an in-place parse of 'text': every object, pair, array and array element
size_t jsonBufferSize(const char* text, size_t length);

// 'text' is NUL terminated and destroyed by the in-place parse; 'out' is garbage unless Ok.
// No exception leaves here (-fexceptions build: std::vector / std::deque throw bad_alloc)
ParseResult parseInPlace(char* text, size_t json_size, const Limits& limits, Config& out,
    String& error, String& warnings);

// the built-in icon names ("mdi:" prefix optional in the config)
size_t iconCount();
const char* iconName(size_t index);

} // namespace nspanel
} // namespace espurna
