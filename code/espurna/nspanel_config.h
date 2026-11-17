/*

NSPANEL MODULE - config model, its compiler and the JSON it gives back

/nspanel.json is compiled once (at its upload, or at the first boot of a board that still keeps the old JSON text) into
an image: fixed records, offsets instead of pointers, one string table, the accepted JSON as a compact tree. The image
is all the module keeps in its flash data (nspanel_storage.cpp) and is read in place through the mapped flash, nothing
of it is copied into RAM (a chip whose data cannot be read as plain memory gets one heap copy of it). RAM keeps only
what changes: the entity states and the few settings of the on-panel tiles. GET /nspanel.json is written back from the
tree, unknown keys are dropped at the upload with a warning.

Depends on Arduino String and ArduinoJson only, so the host tests build it as it is (code/test/unit/src/nspanel)

*/

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "nspanel_icons.h"

namespace espurna {
namespace nspanel {

namespace build {

// a half of the module's flash data (nspanel_storage.cpp, 2 x 16 KB) less its header: the most an upload may send and
// the most its compiled image may take
constexpr size_t ConfigMax { 16384 - 16 };

// ArduinoJson 5 stops at 10 levels by default; each nested "page" adds 3 (item, page, items)
constexpr uint8_t ConfigNestingLimit { 24 };

// the largest heap block a compile takes: the JSON nodes, the string table and the tree grow in blocks of this size
// (a single text longer than it gets a block of its own size), the image goes to its sink as it is laid out
constexpr size_t CompileBlock { 1024 };

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
// Text of the image. Str is a field of a record: an offset from itself into the string table (the image moves as a
// whole, mapped or copied, so the records never hold a pointer). Text is the same seen from RAM (Config)
// -----------------------------------------------------------------------------

struct Text {
    const char* ptr { "" };

    const char* c_str() const {
        return ptr;
    }

    size_t length() const {
        return strlen(ptr);
    }

    operator String() const {
        return String(ptr);
    }

    bool operator==(const char* other) const {
        return 0 == strcmp(ptr, other ? other : "");
    }

    bool operator!=(const char* other) const {
        return !(*this == other);
    }

    bool operator==(const String& other) const {
        return *this == other.c_str();
    }

    bool operator!=(const String& other) const {
        return !(*this == other.c_str());
    }
};

struct Str {
    int16_t rel;

    Str() = default;
    Str(const Str&) = delete;
    Str& operator=(const Str&) = delete;

    const char* c_str() const {
        return reinterpret_cast<const char*>(this) + rel;
    }

    Text text() const {
        return Text{c_str()};
    }

    size_t length() const {
        return strlen(c_str());
    }

    operator String() const {
        return String(c_str());
    }

    bool operator==(const char* other) const {
        return 0 == strcmp(c_str(), other ? other : "");
    }

    bool operator!=(const char* other) const {
        return !(*this == other);
    }

    bool operator==(const String& other) const {
        return *this == other.c_str();
    }

    bool operator!=(const String& other) const {
        return !(*this == other.c_str());
    }

    bool operator==(const Str& other) const {
        return *this == other.c_str();
    }

    bool operator!=(const Str& other) const {
        return !(*this == other.c_str());
    }
};

// a list of the image: offset from itself to its first element, the count
template <typename T>
struct Span {
    int16_t rel;
    uint16_t count;

    Span() = default;
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    const T* begin() const {
        return reinterpret_cast<const T*>(reinterpret_cast<const char*>(this) + rel);
    }

    const T* end() const {
        return begin() + count;
    }

    size_t size() const {
        return count;
    }

    bool empty() const {
        return !count;
    }

    const T& operator[](size_t index) const {
        return begin()[index];
    }
};

// a list of the image seen from RAM (Config)
template <typename T>
struct View {
    const T* data { nullptr };
    size_t count { 0 };

    View() = default;
    View(const Span<T>& span) :
        data(span.begin()),
        count(span.size())
    {}

    const T* begin() const {
        return data;
    }

    const T* end() const {
        return data + count;
    }

    size_t size() const {
        return count;
    }

    bool empty() const {
        return !count;
    }

    const T& operator[](size_t index) const {
        return data[index];
    }
};

// -----------------------------------------------------------------------------
// Config model: the records of the image
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

// what is configured; what it is now (state, value, number) is in RAM: entityLive()
struct Entity {
    Str topic;    // absolute MQTT state topic, empty for local relays
    Str command;  // absolute MQTT command topic, empty: the state topic + mqtt.set (entityCommand())

    // JSON state payloads (zigbee2mqtt, Istabai): the keys into the payload (an array index as its number).
    // Empty: the payload itself is the value. Several entities may share one topic, equal paths share their keys
    Span<Str> path;
    Str on;       // state values for this entity, empty: mqtt.on / mqtt.off
    Str off;
    Str payload;  // command template, "%s" is replaced by the value; empty: the plain value

    // "url" / "on_url" / "off_url": an HTTP GET instead of the MQTT command ("url": the same for on and off)
    Str url_on;
    Str url_off;

    int16_t relay;
};

inline bool samePath(const Span<Str>& a, const Span<Str>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t index = 0; index < a.size(); ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

// the entity now, in RAM
struct EntityLive {
    String value;
    float number { NAN }; // value as a number, NAN when it is not one
    int8_t state { -1 };  // -1 unknown, 0 off, 1 on
};

// "type": "thermostat": entity indices into Config::entities, -1 when not configured
struct Thermostat {
    int16_t current;
    int16_t setpoint;  // also carries the command topic and payload template
    int16_t humidity;
    int16_t active;
    int16_t min;
    int16_t max;
    float min_value;   // limits while min/max are unknown, or fixed numbers from the config
    float max_value;
    float step;
    Str history;       // topic of a retained history series, shown as a chart (empty: none)
    uint16_t reserved;
};

// home info lines (OHpanel info1_* / info2_*, screensaver.info)
struct InfoLine {
    Str icon;
    Str text;
    int16_t entity;
    Str format;        // printf with one float conversion, empty: the value text as is
    int16_t extra;
    Str extra_format;
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
    int16_t entity { -1 };
    int16_t group { -1 };
    int16_t page { -1 };
    Action action { Action::Toggle };
    bool valid { false };
};

// TFT >= 53: an icon text ending in '¬<digit>' switches the grid tile to that font
constexpr uint8_t NoFont { 0xFF };

struct Item {
    Str name;
    Str icon;
    Str history;       // not a thermostat: a tap opens the chart of this retained series instead of switching
    ItemType type;
    uint8_t font;
    int32_t color;     // OHpanel: icon colour (RGB565) when on, -1 = theme accent
    int16_t thermostat;
    Str action;        // a button's "action" of another module (nspanelAction(), nspanel.h), empty: none
    Target target;
};

// an item's "action" that is not toggle / on / off: the name another module registered (nspanelAction())
constexpr size_t ActionNameMax { 24 };

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
    Str id;
    Str title;
    bool settings;     // "type": "settings": the OHpanel settings tiles instead of items
    bool list;         // "type": "list": OHpanel entities_rows (one row per item), others draw a grid
    Grid grid;         // "layout": "4x2" | "3x3" | "3x2" | "2x2" | "1x2"; "columns": 3 = "3x2"
    uint8_t per_screen;// "per_screen": tiles (or list rows) per screen, 0 = as many as fit
    bool draw;         // "draw": true: OHpanel draws the tiles at runtime (fill / cirs / xstr) even for 4x2
    bool nested;       // an item's "page": a sub-page, not in the swipe row, opened by its tile
    Span<uint16_t> items;
};

// "groups": its name and the entities in it
struct Group {
    Str name;
    Span<int16_t> members;
};

// a distinct MQTT state topic of the entities (subscribed once)
struct Topic {
    Str name;
    uint16_t reserved;
};

// the image, at its start. The first 8 bytes stay the same in every layout: an image of an older layout is turned back
// into JSON by its tree and compiled again
struct Image {
    uint16_t layout;        // ImageLayout
    uint16_t tree_version;  // TreeVersion
    uint16_t size;          // bytes of the whole image
    uint16_t tree;          // offset of the root object of the tree

    Str on;
    Str off;
    Str toggle;
    Str set;
    Str relay_labels[2];

    uint16_t timeout;
    uint8_t brightness;
    uint8_t dim;

    bool weather;
    bool temperature;
    bool temperature_show;
    bool beep;

    bool debug;
    DriverPref display;
    lang::Language language;
    uint8_t background;

    uint8_t weather_days;
    Layout layout_default;
    Theme theme;
    uint8_t reserved;

    uint16_t color_on;
    uint16_t color_off;
    uint16_t color_unknown;
    uint16_t reserved2;

    float lat;
    float lon;
    uint32_t refresh;
    int32_t accent;
    int32_t text_color;
    int32_t dim_color;

    Span<Entity> entities;
    Span<Item> items;
    Span<Page> pages;
    Span<Thermostat> thermostats;
    Span<Group> groups;
    Span<uint16_t> shortcuts;     // item indices (home shortcuts, screensaver.shortcuts)
    Span<Target> buttons;
    Span<InfoLine> info;
    Span<Topic> topics;
};

// 2: Item::action (was reserved)
constexpr uint16_t ImageLayout { 2 };
constexpr uint16_t TreeVersion { 1 };

// -----------------------------------------------------------------------------
// The running config: the image (mapped flash or a heap copy), the settings of the on-panel tiles, the entity states
// -----------------------------------------------------------------------------

struct Config {
    Text on { "1" };
    Text off { "0" };
    Text toggle { "2" };
    Text set { "/set" };

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

    // OHpanel ("ohpanel" object); background, layout and theme are also the on-panel settings (applyUiSettings())
    uint8_t background { 0 };     // index into ohpanel::Backgrounds
    int32_t accent { -1 };        // RGB565 overrides of the background's theme, -1 = theme
    int32_t text_color { -1 };
    int32_t dim_color { -1 };
    uint8_t weather_days { 7 };
    Text relay_labels[2];
    Layout layout { Layout::Center }; // ohpanel.layout; the nspIcons setting wins
    Theme theme { Theme::Dark };      // ohpanel.theme; the nspTheme setting wins

    View<uint16_t> shortcuts;
    View<InfoLine> info;
    size_t info_count { 0 };
    View<Thermostat> thermostats;
    View<Entity> entities;
    View<Group> groups;
    View<Item> items;
    View<Page> pages;
    View<Target> buttons;
    View<Topic> topics;

    // the entity states, one per entity
    std::vector<EntityLive> live;

    // the image, nullptr before bind()
    const Image* image { nullptr };

    // the heap copy of the image when it is not read from the mapped flash
    std::unique_ptr<uint8_t[]> owned;
};

// the MQTT command topic of the entity: its "command", else the state topic + mqtt.set; empty - none
inline String entityCommand(const Config& config, const Entity& entity) {
    if (entity.command.length()) {
        return String(entity.command.c_str());
    }

    String out;
    if (entity.topic.length()) {
        out = entity.topic.c_str();
        out += config.set.c_str();
    }
    return out;
}

// the records of 'data' (an image of 'size' bytes): false - not an image of this layout. 'owned' is the heap block of
// it, if any (kept by the Config)
bool bind(Config& out, const uint8_t* data, size_t size, std::unique_ptr<uint8_t[]>& owned);

// -----------------------------------------------------------------------------
// Compiler
// -----------------------------------------------------------------------------

// what the parse checks against the rest of the firmware
struct Limits {
    size_t relays { 0 };       // "relay" indices below this are valid
    size_t mqtt_buffer { 0 };  // the mqttBuffer setting, 0 without MQTT
};

enum class ParseResult {
    Ok,
    Invalid,
    LowMemory,
    WriteFailed,  // the sink did not take the image
};

// Where compile() puts the image as it is laid out: begin() with its size once the draft is complete, then all its
// bytes in order (write(), pieces of any size), then end(). A false stops the compile, 'error' says why (an "out of
// memory" one is ParseResult::LowMemory)
struct ImageSink {
    virtual bool begin(size_t size, String& error) = 0;
    virtual bool write(const uint8_t* data, size_t length) = 0;
    virtual bool end(String& error) = 0;

protected:
    ~ImageSink() = default;
};

// a compiled image in one heap block (the built-in config, the host tests); the saved config goes straight to the
// flash (storage::save())
struct ImageBlock : public ImageSink {
    std::unique_ptr<uint8_t[]> data;
    size_t size { 0 };

    bool begin(size_t length, String& error) override;
    bool write(const uint8_t* bytes, size_t length) override;
    bool end(String& error) override;

private:
    size_t filled { 0 };
};

// Exact node storage of an in-place parse of 'text': every object, pair, array and array element (the heap estimate of
// a compile, the nodes themselves go into blocks of build::CompileBlock)
size_t jsonBufferSize(const char* text, size_t length);

// 'text' is NUL terminated and destroyed by the in-place parse; 'out' gets the image only once the draft is complete,
// nothing of it unless the result is Ok or WriteFailed. No heap block above build::CompileBlock (but a longer single
// text). No exception leaves here (-fexceptions build: std::deque / std::vector throw bad_alloc)
ParseResult compile(char* text, const Limits& limits, ImageSink& out, String& error, String& warnings);

// the image is of an older layout: its tree can still be written back as JSON (and compiled again)
bool treeReadable(const uint8_t* data, size_t size);

// The JSON of the image's tree: 2 space indent, the keys in the order of the upload. 'write' gets the text piece by
// piece; returns the length
using JsonWriter = void (*)(void* ctx, const char* data, size_t length);
size_t writeJson(const uint8_t* data, JsonWriter write, void* ctx);

// the built-in icon names ("mdi:" prefix optional in the config)
size_t iconCount();
const char* iconName(size_t index);

} // namespace nspanel
} // namespace espurna
