/*

THE PAGES OF THE SCREEN MODULE - the light display

The tile pages of /nspanel.json drawn by the screen module itself on its pixel panel (an M5Stack's TFT of the lcd
module, any SPI TFT, an OLED): no touch, three keys (SCREEN.KEY prev / enter / next / back: the M5Stack's buttons A, B,
C with a terminal command action, the WebUI keys of the SCREEN panel).

- home: the clock, the date, the weather now (the config's "weather"), the room temperature, the info lines
- the cards of the swipe row: the tiles of a page in its "layout" (4x2, 3x3, 3x2, 2x2, 1x2; a small screen: 2 a card),
  a frame around the chosen one; prev / next move it, past the ends to the card before / after (and home), enter taps
  it (a switch toggles, a button presses, a scene, a page opens, a thermostat opens its card), back goes home (a
  sheet of a nested page: back to where it was opened)
- a thermostat: the room and the target temperature, prev / next change the target by its step, enter or back send it
  and close the card (the change is sent also 3 s after the last key)
- blanking and brightness of the config's "screensaver" (timeout, brightness, dim) through the screen module

What the light display does not have of the OHpanel's: the weather forecast page, the history charts, the settings
tiles, the backgrounds and themes (a dark one; a 1-bit panel: white on black).

*/

#include "espurna.h"

#if SCREEN_SUPPORT

#include "screen.h"
#include "screen_pages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace espurna {
namespace pages {
namespace light {
namespace {

constexpr uint32_t ThermostatSendMs { 3000 };

enum class View {
    Home,
    Card,
    Thermostat,
};

struct Palette {
    uint16_t back;
    uint16_t header;
    uint16_t tile;
    uint16_t tile_on;
    uint16_t text;
    uint16_t text_dim;
    uint16_t text_on;
    uint16_t icon;
    uint16_t focus;
};

constexpr Palette Colour {
    screenRgb(12, 14, 18),
    screenRgb(30, 34, 42),
    screenRgb(46, 52, 62),
    screenRgb(236, 236, 236),
    ScreenWhite,
    screenRgb(150, 156, 168),
    screenRgb(24, 24, 24),
    screenRgb(176, 182, 194),
    screenRgb(0, 160, 255),
};

constexpr Palette Mono {
    ScreenBlack,
    ScreenBlack,
    ScreenBlack,
    ScreenWhite,
    ScreenWhite,
    ScreenWhite,
    ScreenBlack,
    ScreenWhite,
    ScreenWhite,
};

struct State {
    std::vector<Card> cards;
    View view { View::Home };
    int card { -1 };
    int back_card { -1 };       // a nested page's sheet: the card it was opened from (-1 home)
    size_t focus { 0 };

    // a thermostat card: its item and the target being changed
    int thermostat { -1 };
    float target { NAN };
    bool target_unsent { false };
    uint32_t target_at { 0 };

    bool full { true };
    uint32_t tiles { 0 };       // the tiles of the card to look at again
    bool body { false };        // home / thermostat: its parts to look at again
    bool header { false };
    int minute { -1 };

    // what each part shows now: a part is drawn again only when its text changes (a full draw forgets them)
    struct Shown {
        espurna::string title;
        espurna::string time;
        int position { -2 };
        espurna::string clock;
        espurna::string weekday;
        espurna::string weather;
        espurna::string room;
        espurna::string info[InfoLinesMax];
        espurna::string now;
        espurna::string target;
        espurna::string status;
        espurna::string tiles[TilesMax];
    } shown;
};

State state;

const Palette& palette() {
    return screenColour() ? Colour : Mono;
}

bool small() {
    return screenHeight() < 120;
}

int header_height() {
    return small() ? 0 : 24;
}

void invalidate() {
    screenInvalidate();
}

// -----------------------------------------------------------------------------
// What a tile shows
// -----------------------------------------------------------------------------

// the first code point of a UTF-8 icon text (the TFT's code points of screen_pages_icons.h), 0 none
uint16_t icon_code(const char* text) {
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    if (!p || !*p) {
        return 0;
    }
    if (*p < 0x80) {
        return 0;
    }
    if (((p[0] & 0xe0) == 0xc0) && p[1]) {
        return static_cast<uint16_t>(((p[0] & 0x1f) << 6) | (p[1] & 0x3f));
    }
    if (((p[0] & 0xf0) == 0xe0) && p[1] && p[2]) {
        return static_cast<uint16_t>(((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f));
    }
    return 0;
}

uint16_t default_icon(ItemType type) {
    switch (type) {
    case ItemType::Switch:
        return icons::LightSwitch;
    case ItemType::Button:
        return icons::GestureTapButton;
    case ItemType::Scene:
        return icons::LightbulbGroup;
    case ItemType::Text:
        return icons::Thermometer;
    case ItemType::Link:
        return icons::ArrowRightBold;
    case ItemType::Thermostat:
        return icons::HomeThermometerOutline;
    }

    return icons::CircleMedium;
}

const Thermostat* thermostat_of(const Item& item) {
    if ((item.thermostat < 0) || (static_cast<size_t>(item.thermostat) >= config().thermostats.size())) {
        return nullptr;
    }
    return &config().thermostats[item.thermostat];
}

espurna::string degrees(float value) {
    if (std::isnan(value)) {
        return "--";
    }
    return temperatureShort(value) + "\xC2\xB0";
}

struct Face {
    bool active { false };
    espurna::string value;
};

Face face_of(size_t index) {
    Face out;
    const auto& item = config().items[index];

    switch (item.type) {
    case ItemType::Switch: {
        const auto value = entityState(item.target.entity);
        out.active = (value == 1);
        out.value = (value == 1) ? lang::texts().on : (value == 0) ? lang::texts().off : "--";
        break;
    }

    case ItemType::Button:
        out.active = (entityState(item.target.entity) == 1);
        break;

    case ItemType::Scene: {
        const auto value = groupState(item.target.group);
        out.active = (value == 1);
        if (value == 1) {
            out.value = espurna::string(lang::texts().on_count) + groupOnCount(item.target.group);
        } else {
            out.value = (value == 0) ? lang::texts().off : "--";
        }
        break;
    }

    case ItemType::Text:
        if (item.target.entity >= 0) {
            out.value = entityValue(item.target.entity);
        }
        break;

    case ItemType::Link:
        break;

    case ItemType::Thermostat: {
        const auto* thermostat = thermostat_of(item);
        if (thermostat) {
            out.active = thermostatActive(*thermostat);
            out.value = degrees(entityNumber(thermostat->current)) + " \xC2\xBB "
                + degrees(entityNumber(thermostat->setpoint));
        }
        break;
    }
    }

    return out;
}

// -----------------------------------------------------------------------------
// Cards and tiles
// -----------------------------------------------------------------------------

void build() {
    buildCards(state.cards, small() ? 2 : 8, 40, false, 0, !small());
}

size_t row_cards() {
    return rowCards(state.cards);
}

const Card* current_card() {
    if ((state.card < 0) || (static_cast<size_t>(state.card) >= state.cards.size())) {
        return nullptr;
    }
    return &state.cards[state.card];
}

int item_of(const Card& card, size_t n) {
    const auto& page = config().pages[card.page];
    const size_t position = card.first + n;
    if (position >= page.items.size()) {
        return -1;
    }
    return page.items[position];
}

struct Grid {
    int cols;
    int rows;
};

Grid grid_of(const Card& card) {
    if (small()) {
        return {2, 1};
    }
    return {gridCols(card.grid), gridRows(card.grid)};
}

struct Box {
    int x;
    int y;
    int w;
    int h;
};

Box tile_box(const Card& card, size_t n) {
    const auto grid = grid_of(card);
    const int gap = small() ? 2 : 4;
    const int top = header_height() + gap;
    const int width = screenWidth() - 2 * gap;
    const int height = screenHeight() - top - gap;
    const int w = (width - (grid.cols - 1) * gap) / grid.cols;
    const int h = (height - (grid.rows - 1) * gap) / grid.rows;
    const int col = static_cast<int>(n) % grid.cols;
    const int row = static_cast<int>(n) / grid.cols;
    return {gap + col * (w + gap), top + row * (h + gap), w, h};
}

void draw_tile(const Card& card, size_t n) {
    const auto box = tile_box(card, n);
    const auto& colours = palette();
    const int index = item_of(card, n);
    const bool focused = (n == state.focus);

    if (index < 0) {
        if ((n < TilesMax) && !state.shown.tiles[n].equals("none")) {
            state.shown.tiles[n] = "none";
            screenFill(box.x, box.y, box.w, box.h, colours.back);
        }
        return;
    }

    const auto& item = config().items[index];
    const auto face = face_of(index);
    const bool colour = screenColour();

    // the same as on the screen: nothing to draw
    if (n < TilesMax) {
        auto key = espurna::string(face.active ? "1" : "0") + (focused ? "f" : "-") + face.value;
        if (key.equals(state.shown.tiles[n])) {
            return;
        }
        state.shown.tiles[n] = std::move(key);
    }

    // the tile: filled when on (a 1-bit panel: an outline when off)
    const int inset = colour ? 0 : 2;
    if (!colour || face.active) {
        screenFill(box.x, box.y, box.w, box.h, colours.back);
    }
    if (face.active) {
        screenFill(box.x + inset, box.y + inset, box.w - 2 * inset, box.h - 2 * inset, colours.tile_on);
    } else if (colour) {
        screenFill(box.x, box.y, box.w, box.h, colours.tile);
    } else {
        screenFrame(box.x + inset, box.y + inset, box.w - 2 * inset, box.h - 2 * inset, 1, colours.text);
    }

    const uint16_t back = face.active ? colours.tile_on : colours.tile;
    const uint16_t text = face.active ? colours.text_on : colours.text;
    const uint16_t dim = face.active ? colours.text_on : colours.text_dim;
    uint16_t icon = face.active
        ? ((item.color >= 0) ? static_cast<uint16_t>(item.color) : config().color_on)
        : colours.icon;
    if (!colour) {
        icon = text;
    }

    const auto name_font = (box.w >= 96) ? ScreenFont::Normal : ScreenFont::Small;
    const int name_h = screenFontHeight(name_font);
    const int value_h = face.value.length() ? screenFontHeight(ScreenFont::Small) : 0;
    int icon_size = ((box.w >= 60) && (box.h >= 48 + name_h + value_h + 8)) ? 48 : 24;
    if (box.h < 24 + name_h + 4) {
        icon_size = 0;
    }

    const int block = icon_size + (icon_size ? 2 : 0) + name_h + value_h;
    int y = box.y + std::max(2, (box.h - block) / 2);
    if (icon_size) {
        const int x = box.x + (box.w - icon_size) / 2;
        uint16_t code = icon_code(item.icon.c_str());
        if (!code || !screenIcon(x, y, icon_size, code, icon, back)) {
            if (!screenIcon(x, y, icon_size, default_icon(item.type), icon, back)) {
                // the first letter of the name (a whole UTF-8 sequence)
                char first[5] {};
                const char* name = item.name.c_str();
                const auto lead = static_cast<uint8_t>(name[0]);
                const size_t length = (lead < 0x80) ? 1 : ((lead & 0xe0) == 0xc0) ? 2 : ((lead & 0xf0) == 0xe0) ? 3 : 1;
                std::strncpy(first, name, length);
                screenText(x, y, icon_size, icon_size, ScreenFont::Large, icon, ScreenNoBack, ScreenAlign::Center, first);
            }
        }
        y += icon_size + 2;
    }

    screenText(box.x + 2, y, box.w - 4, name_h, name_font, text, ScreenNoBack, ScreenAlign::Center, item.name.c_str());
    y += name_h;
    if (value_h) {
        screenText(box.x + 2, y, box.w - 4, value_h, ScreenFont::Small, dim, ScreenNoBack, ScreenAlign::Center,
            face.value.c_str());
    }

    if (focused) {
        screenFrame(box.x, box.y, box.w, box.h, colour ? 3 : 1, colour ? colours.focus : (face.active ? ScreenBlack : ScreenWhite));
    }
}

// -----------------------------------------------------------------------------
// Header, home, thermostat
// -----------------------------------------------------------------------------

bool local_time(tm& out) {
    const auto now = ::time(nullptr);
    if (now < 1000000000) {
        return false;
    }
    localtime_r(&now, &out);
    return true;
}

espurna::string clock_text() {
    tm t{};
    if (!local_time(t)) {
        return "--:--";
    }
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", t.tm_hour, t.tm_min);
    return buffer;
}

void draw_header(const char* title, bool full) {
    if (small()) {
        return;
    }

    auto& shown = state.shown;
    const auto& colours = palette();
    const int w = screenWidth();
    const int h = header_height();
    if (full) {
        screenFill(0, 0, w, h, colours.header);
        shown.title = espurna::string();
        shown.time = espurna::string();
        shown.position = -2;
    }

    // the time on the right, as wide as "88:88" always (the dots stay where they are)
    const int time_w = screenTextWidth(ScreenFont::Normal, "88:88") + 8;
    const auto time = clock_text();
    if (!time.equals(shown.time)) {
        shown.time = time;
        screenText(w - time_w, 0, time_w - 6, h, ScreenFont::Normal, colours.text, colours.header, ScreenAlign::Right,
            time.c_str());
    }

    const size_t count = row_cards() + 1;
    const int dots = ((count > 1) && (count <= 12)) ? static_cast<int>(count) * 7 : 0;
    if (!shown.title.equals(title)) {
        shown.title = title;
        screenText(6, 0, w - time_w - dots - 14, h, ScreenFont::Normal, colours.text, colours.header,
            ScreenAlign::Left, title);
    }

    // where the card is in the row: a dot each (home first)
    const int position = (state.view == View::Home) ? 0 : (state.card + 1);
    if (dots && (position != shown.position)) {
        shown.position = position;
        const int x0 = w - time_w - 8 - dots;
        screenFill(x0, 0, dots, h, colours.header);
        for (size_t n = 0; n < count; ++n) {
            const bool here = (static_cast<int>(n) == position);
            screenFill(x0 + static_cast<int>(n) * 7, h / 2 - (here ? 2 : 1), here ? 4 : 2, here ? 4 : 2,
                here ? colours.text : colours.text_dim);
        }
    }
}

espurna::string info_text(const InfoLine& line) {
    espurna::string text = line.text.c_str();
    const auto* live = entityLive(line.entity);
    if (live) {
        espurna::string value;
        if (!live->value.length()) {
            value = "--";
        } else if (line.format.length() && !std::isnan(live->number)) {
            char buffer[40];
            snprintf(buffer, sizeof(buffer), line.format.c_str(), static_cast<double>(live->number));
            value = buffer;
        } else {
            value = live->value;
        }
        if (text.length()) {
            text += "  ";
        }
        text += value;
    }
    return text;
}

void draw_home(bool full) {
    auto& shown = state.shown;
    const auto& colours = palette();
    const int w = screenWidth();
    const int h = screenHeight();
    const int top = header_height();

    tm t{};
    const bool time_known = local_time(t);
    const auto time = clock_text();

    if (full) {
        screenFill(0, top, w, h - top, colours.back);
        shown.clock = espurna::string();
        shown.weekday = espurna::string();
        shown.weather = espurna::string();
        shown.room = espurna::string();
        for (auto& info : shown.info) {
            info = espurna::string();
        }
    }

    if (small()) {
        if (!time.equals(shown.clock)) {
            shown.clock = time;
            screenText(0, 0, w, h / 2 + 8, ScreenFont::Big, colours.text, colours.back, ScreenAlign::Center,
                time.c_str());
        }
        const auto date = time_known ? lang::dateShort(t) : espurna::string();
        if (!date.equals(shown.weekday)) {
            shown.weekday = date;
            screenText(0, h / 2 + 8, w, h / 2 - 8, ScreenFont::Small, colours.text, colours.back, ScreenAlign::Center,
                date.c_str());
        }
        return;
    }

    draw_header(time_known ? lang::dateShort(t).c_str() : "", full);

    // the clock and the weekday on the left
    const int clock_h = screenFontHeight(ScreenFont::Huge);
    const int normal = screenFontHeight(ScreenFont::Normal);
    const int left_w = w / 2 + 20;
    if (!time.equals(shown.clock)) {
        shown.clock = time;
        screenText(10, top + 6, left_w, clock_h, ScreenFont::Huge, colours.text, colours.back, ScreenAlign::Left,
            time.c_str());
    }

    int y = top + 6 + clock_h;
    const espurna::string weekday = time_known ? lang::weekdayFull(t.tm_wday) : "";
    if (!weekday.equals(shown.weekday)) {
        shown.weekday = weekday;
        screenText(12, y, left_w, normal, ScreenFont::Normal, colours.text_dim, colours.back, ScreenAlign::Left,
            weekday.c_str());
    }
    y += normal;

    // the weather now on the right: as large as the room on the right of the clock takes it (96 px on 320, else 48)
    const auto& weather = weather::current();
    const int size = (w - 170 >= 96 + 8) ? 96 : 48;
    const int weather_x = w - 8 - size - 24;
    const int weather_w = w - weather_x;
    const int weather_h = size + 2 + screenFontHeight(ScreenFont::Big);
    espurna::string key;
    char temperature[16] {};
    if (config().weather && weather.valid) {
        snprintf(temperature, sizeof(temperature), "%.0f\xC2\xB0", static_cast<double>(weather.temperature));
        key = espurna::string(weather.code) + (weather.day ? "d" : "n") + temperature;
    } else if (config().weather) {
        key = "-";
    }

    if (!key.equals(shown.weather)) {
        shown.weather = key;
        screenFill(weather_x, top + 2, weather_w, weather_h, colours.back);
        if (config().weather && weather.valid) {
            const auto icon = icons::weather(weather.code, weather.day);
            screenIcon(w - 8 - size, top + 2, size, icon.icon, screenColour() ? icon.color : colours.text,
                colours.back);
            screenText(weather_x, top + 2 + size, weather_w - 8, screenFontHeight(ScreenFont::Big), ScreenFont::Big,
                colours.text, colours.back, ScreenAlign::Center, temperature);
        } else if (config().weather) {
            screenText(weather_x, top + 20, weather_w - 8, screenFontHeight(ScreenFont::Small), ScreenFont::Small,
                colours.text_dim, colours.back, ScreenAlign::Right, lang::texts().no_weather);
        }
    }

    // the room temperature
    const auto room = ntc::text();
    y += 4;
    if (!room.equals(shown.room)) {
        shown.room = room;
        screenFill(10, y, weather_x - 10, 24, colours.back);
        if (room.length()) {
            screenIcon(10, y, 24, icons::HomeThermometerOutline, colours.icon, colours.back);
            screenText(40, y, weather_x - 44, 24, ScreenFont::Large, colours.text, colours.back, ScreenAlign::Left,
                room.c_str());
        }
    }

    // the info lines at the bottom
    const size_t lines = std::min<size_t>(config().info_count, InfoLinesMax);
    int line_y = h - static_cast<int>(lines) * 28 - 4;
    for (size_t n = 0; n < lines; ++n) {
        const auto& line = config().info[n];
        const auto text = info_text(line);
        if (!text.equals(shown.info[n])) {
            shown.info[n] = text;
            int x = 10;
            const auto code = icon_code(line.icon.c_str());
            if (code && screenIcon(x, line_y + 2, 24, code, colours.icon, colours.back)) {
                x += 30;
            }
            screenText(x, line_y, w - x - 10, 28, ScreenFont::Normal, colours.text, colours.back, ScreenAlign::Left,
                text.c_str());
        }
        line_y += 28;
    }
}

float shown_target(const Thermostat& thermostat) {
    return std::isnan(state.target) ? entityNumber(thermostat.setpoint) : state.target;
}

void draw_thermostat(bool full) {
    const auto& colours = palette();
    const int w = screenWidth();
    const int h = screenHeight();
    const int top = header_height();

    if ((state.thermostat < 0) || (static_cast<size_t>(state.thermostat) >= config().items.size())) {
        return;
    }

    const auto& item = config().items[state.thermostat];
    const auto* thermostat = thermostat_of(item);
    if (!thermostat) {
        return;
    }

    auto& shown = state.shown;
    draw_header(item.name.c_str(), full);
    if (full) {
        screenFill(0, top, w, h - top, colours.back);
        shown.now = espurna::string();
        shown.target = espurna::string();
        shown.status = espurna::string();
    }

    const bool heating = thermostatActive(*thermostat);
    const int big = screenFontHeight(ScreenFont::Big);
    const int huge = screenFontHeight(ScreenFont::Huge);
    const int normal = screenFontHeight(ScreenFont::Normal);
    const int icon = small() ? 0 : 48;

    // the room now, the target big, and what the keys do under it
    int y = top + (small() ? 0 : 8);
    if (!small()) {
        espurna::string now = degrees(entityNumber(thermostat->current));
        const float humidity = entityNumber(thermostat->humidity);
        if (!std::isnan(humidity)) {
            now += "  ";
            now += espurna::string(static_cast<long>(lroundf(humidity)));
            now += '%';
        }
        if (!now.equals(shown.now)) {
            shown.now = now;
            screenText(0, y, w, big, ScreenFont::Big, colours.text_dim, colours.back, ScreenAlign::Center, now.c_str());
        }
        y += big + 4;
    }

    const auto target = degrees(shown_target(*thermostat));
    const auto font = small() ? ScreenFont::Big : ScreenFont::Huge;
    const int target_h = small() ? big : huge;
    const uint16_t target_colour = heating && screenColour() ? screenRgb(255, 140, 40) : colours.text;
    const auto target_key = target + (heating ? "+" : "-");
    if (!target_key.equals(shown.target)) {
        shown.target = target_key;
        screenText(0, y, w, target_h, font, target_colour, colours.back, ScreenAlign::Center, target.c_str());
    }
    y += target_h;

    if (!small()) {
        espurna::string state_text = lang::texts().target;
        state_text += target;
        if (heating) {
            state_text += " \xC2\xB7 ";
            state_text += lang::texts().heating;
        }
        if (!state_text.equals(shown.status)) {
            shown.status = state_text;
            screenText(0, y, w, normal, ScreenFont::Normal, colours.text_dim, colours.back, ScreenAlign::Center,
                state_text.c_str());
        }
    }

    // minus / OK / plus over the keys (the M5Stack's A, B, C sit under these thirds of the screen)
    if (icon && full) {
        const int bottom = h - icon - 4;
        screenIcon(w / 6 - icon / 2, bottom, icon, icons::Minus, colours.text, colours.back);
        screenIcon(w / 2 - icon / 2, bottom, icon, icons::Thermometer, colours.text_dim, colours.back);
        screenIcon(5 * w / 6 - icon / 2, bottom, icon, icons::Plus, colours.text, colours.back);
    }
}

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------

void draw_card(bool full) {
    const auto* card = current_card();
    if (!card) {
        state.view = View::Home;
        draw_home(true);
        return;
    }

    if (full) {
        screenFill(0, 0, screenWidth(), screenHeight(), palette().back);
        for (auto& tile : state.shown.tiles) {
            tile = espurna::string();
        }
    }

    draw_header(card->title.c_str(), full);
    for (size_t n = 0; (n < card->count) && (n < TilesMax); ++n) {
        if (full || (state.tiles & (1u << n))) {
            draw_tile(*card, n);
        }
    }
}

void draw(bool full) {
    full = full || state.full;
    switch (state.view) {
    case View::Home:
        if (full || state.body || state.header) {
            draw_home(full);
        }
        break;

    case View::Card:
        draw_card(full);
        break;

    case View::Thermostat:
        if (full || state.body || state.header) {
            draw_thermostat(full);
        }
        break;
    }

    state.full = false;
    state.body = false;
    state.header = false;
    state.tiles = 0;
}

void redraw() {
    state.full = true;
    invalidate();
}

// -----------------------------------------------------------------------------
// The keys
// -----------------------------------------------------------------------------

void send_target() {
    if (!state.target_unsent || (state.thermostat < 0)) {
        return;
    }

    const auto* thermostat = thermostat_of(config().items[state.thermostat]);
    if (thermostat && !std::isnan(state.target)) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(state.target));
        entitySend(thermostat->setpoint, buffer);
    }

    state.target_unsent = false;
}

void show_home() {
    state.view = View::Home;
    state.card = -1;
    state.back_card = -1;
    redraw();
}

void show_card(int card, size_t focus) {
    if ((card < 0) || (static_cast<size_t>(card) >= state.cards.size())) {
        show_home();
        return;
    }

    state.view = View::Card;
    state.card = card;
    state.focus = std::min<size_t>(focus, state.cards[card].count ? state.cards[card].count - 1 : 0);
    redraw();
}

void close_thermostat() {
    send_target();
    state.thermostat = -1;
    state.target = NAN;
    state.view = View::Card;
    redraw();
}

void open(const Item& item, size_t index) {
    switch (item.type) {
    case ItemType::Link: {
        const int page = item.target.page;
        if (page >= 0) {
            const int card = firstCard(state.cards, page);
            if (card >= 0) {
                state.back_card = state.cards[card].sub ? state.card : -1;
                show_card(card, 0);
            }
        } else {
            show_home();
        }
        break;
    }

    case ItemType::Thermostat:
        state.view = View::Thermostat;
        state.thermostat = static_cast<int>(index);
        state.target = NAN;
        state.target_unsent = false;
        redraw();
        break;

    case ItemType::Switch:
    case ItemType::Button:
    case ItemType::Scene:
    case ItemType::Text:
        itemTap(index, -1);
        state.tiles |= 1u << state.focus;
        invalidate();
        break;
    }
}

void step_target(int direction) {
    if (state.thermostat < 0) {
        return;
    }

    const auto* thermostat = thermostat_of(config().items[state.thermostat]);
    if (!thermostat) {
        return;
    }

    float base = shown_target(*thermostat);
    if (std::isnan(base)) {
        base = entityNumber(thermostat->current);
    }
    if (std::isnan(base)) {
        base = (thermostatMin(*thermostat) + thermostatMax(*thermostat)) / 2.0f;
    }

    buzzer::beep();
    state.target = thermostatClamp(*thermostat, base + static_cast<float>(direction) * thermostat->step);
    state.target_unsent = true;
    state.target_at = espurna::time::ms();
    state.body = true;
    invalidate();
}

void move(int direction) {
    const int row = static_cast<int>(row_cards());

    if (state.view == View::Home) {
        if (!row) {
            return;
        }
        if (direction > 0) {
            show_card(0, 0);
        } else {
            show_card(row - 1, 32);
        }
        return;
    }

    const auto* card = current_card();
    if (!card) {
        show_home();
        return;
    }

    const int focus = static_cast<int>(state.focus) + direction;
    if ((focus >= 0) && (focus < static_cast<int>(card->count))) {
        state.tiles |= (1u << state.focus) | (1u << focus);
        state.focus = static_cast<size_t>(focus);
        invalidate();
        return;
    }

    // past the end of the card: the next / previous one (a nested page's sheets among themselves)
    int next = state.card + direction;
    if (card->sub) {
        if ((next >= 0) && (static_cast<size_t>(next) < state.cards.size()) && samePage(state.cards, state.card, next)) {
            show_card(next, (direction > 0) ? 0 : 32);
        }
        return;
    }

    if ((next < 0) || (next >= row)) {
        show_home();
        return;
    }

    show_card(next, (direction > 0) ? 0 : 32);
}

void key(ScreenKey key) {
    switch (state.view) {
    case View::Thermostat:
        switch (key) {
        case ScreenKey::Prev:
            step_target(-1);
            break;
        case ScreenKey::Next:
            step_target(1);
            break;
        case ScreenKey::Enter:
        case ScreenKey::Back:
            close_thermostat();
            break;
        }
        return;

    case View::Home:
        switch (key) {
        case ScreenKey::Prev:
            move(-1);
            break;
        case ScreenKey::Next:
        case ScreenKey::Enter:
            move(1);
            break;
        case ScreenKey::Back:
            redraw();
            break;
        }
        return;

    case View::Card:
        break;
    }

    switch (key) {
    case ScreenKey::Prev:
        move(-1);
        break;
    case ScreenKey::Next:
        move(1);
        break;
    case ScreenKey::Enter: {
        const auto* card = current_card();
        const int index = card ? item_of(*card, state.focus) : -1;
        if (index >= 0) {
            open(config().items[index], static_cast<size_t>(index));
        }
        break;
    }
    case ScreenKey::Back:
        if (state.back_card >= 0) {
            const int back = state.back_card;
            state.back_card = -1;
            show_card(back, 0);
        } else {
            show_home();
        }
        break;
    }
}

const ScreenContent Content {
    "pages",
    draw,
    key,
};

// -----------------------------------------------------------------------------
// The driver and the display of the pages
// -----------------------------------------------------------------------------

class LightDriver final : public Driver {
public:
    const char* name() const override {
        return "light";
    }

    void begin() override {
        apply();
        screenContent(&Content);
        announceDriver(*this, "1", screenPanelName());
    }

    void loop() override {
        const auto now = espurna::time::ms();
        if (state.target_unsent && (now - state.target_at >= ThermostatSendMs)) {
            send_target();
        }

        // the clock of the header and of home
        tm t{};
        if (local_time(t) && (t.tm_min != state.minute)) {
            state.minute = t.tm_min;
            state.header = true;
            if (state.view == View::Home) {
                state.body = true;
            }
            invalidate();
        }
    }

    bool connected() const override {
        return screenReady();
    }

    bool lost() const override {
        return false;
    }

    const char* version() const override {
        return "1";
    }

    const char* model() const override {
        return screenPanelName();
    }

    void entityChanged(int entity) override {
        switch (state.view) {
        case View::Home:
            for (size_t n = 0; n < std::min<size_t>(config().info_count, InfoLinesMax); ++n) {
                if (config().info[n].entity == entity) {
                    state.body = true;
                }
            }
            break;

        case View::Thermostat:
            state.body = true;
            break;

        case View::Card: {
            const auto* card = current_card();
            if (!card) {
                break;
            }
            for (size_t n = 0; n < card->count; ++n) {
                const int index = item_of(*card, n);
                if ((index >= 0) && itemUses(config().items[index], entity)) {
                    state.tiles |= 1u << n;
                }
            }
            break;
        }
        }

        if (state.body || state.tiles) {
            invalidate();
        }
    }

    void weatherChanged() override {
        if (state.view == View::Home) {
            state.body = true;
            invalidate();
        }
    }

    void temperatureChanged() override {
        weatherChanged();
    }

    void reload() override {
        const auto view = state.view;
        const int card = state.card;
        const size_t focus = state.focus;
        apply();
        if ((view == View::Card) && (card >= 0) && (static_cast<size_t>(card) < state.cards.size())) {
            show_card(card, focus);
        } else {
            show_home();
        }
    }

    void showPage(int page) override {
        if (page >= 0) {
            const int card = firstCard(state.cards, page);
            if (card >= 0) {
                show_card(card, 0);
                return;
            }
        }
        show_home();
    }

    void send(espurna::string) override {
        DEBUG_MSG_P(PSTR("[PAGES] The light display takes no commands\n"));
    }

    void info(::espurna::Output& out) override {
        out.printf_P(PSTR("Light: panel %s %dx%d, %u cards, view %s, card %d, tile %u\n"), screenPanelName(),
            screenWidth(), screenHeight(), static_cast<unsigned>(state.cards.size()),
            (state.view == View::Home) ? "home" : (state.view == View::Card) ? "card" : "thermostat",
            state.card, static_cast<unsigned>(state.focus));
    }

private:
    void apply() {
        build();
        screenSleep(config().timeout, config().brightness, config().dim);
        state.thermostat = -1;
        state.target = NAN;
        state.target_unsent = false;
        state.minute = -1;
    }
};

LightDriver light_driver;

class LightDisplay final : public Display {
public:
    const char* name() const override {
        return "light";
    }

    Driver* active() override {
        return _started ? &light_driver : nullptr;
    }

    void probe(const char*) override {
        if (!_started) {
            _started = true;
            light_driver.begin();
            return;
        }
        light_driver.reload();
    }

    void loop() override {
        if (_started) {
            light_driver.loop();
        }
    }

    void info(::espurna::Output& out) override {
        if (_started) {
            light_driver.info(out);
        } else {
            out.print("Light: not started\n");
        }
    }

private:
    bool _started { false };
};

LightDisplay light_display;

} // namespace

Display& display() {
    return light_display;
}

} // namespace light
} // namespace pages
} // namespace espurna

#endif // SCREEN_SUPPORT
