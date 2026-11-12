/*

NSPANEL MODULE

Sonoff NSPanel (gen1 EU, ESP32) backend with two display drivers, chosen by JSON "display"
("auto" detects the TFT at boot, after a TFT upload and when the display goes silent):
- nspanel_ohpanel.cpp: OHpanel TFT (bc629e1e), plain Nextion commands at 921600, swipes.
  Protocol reference: nspanel/ohpanel-protocol.md
- nspanel_lovelace.cpp: joBr99 "NSPanel Lovelace UI" TFT, 55 BB frames at 115200.
  Protocol reference: nspanel/lovelace-protocol.md

Shared core (the parts: nspanel_core.h):
- this file: the running config, entities, cards, screen texts, room temperature (ESPurna NTC
  sensor), buzzer, relays and hardware buttons, terminal commands, setup and loop
- nspanel_config.cpp: config model from /nspanel.json: pages of tiles, groups/scenes, shortcuts, buttons
- nspanel_storage.cpp: the config in the flash data of the module (modulesData(), every chip)
- nspanel_net.cpp: MQTT state map (absolute state topics), Open-Meteo weather task, "url" requests
- nspanel_link.cpp: UART2 (TX GPIO16, RX GPIO17) ownership, display detection (link::)
- nspanel_flash.cpp: Nextion TFT upload (flash::)
- nspanel_web.cpp: /nspanel and /nspanel.json web endpoints, WebUI screen mirror

Flash layout, `safeboot`, crash-loop guard and updates from the release list: arch/esp32/safeboot_esp32.cpp, as on
every ESP32 image

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "nspanel.h"
#include "nspanel_core.h"

#if BUTTON_SUPPORT
#include "button.h"
#endif

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#if RELAY_SUPPORT
#include "relay.h"
#endif

#if SENSOR_SUPPORT
#include "sensor.h"
#endif

#include "buzzer_chip.h"

#include <cmath>
#include <ctime>
#include <memory>
#include <new>
#include <vector>

namespace espurna {
namespace nspanel {
namespace {

// the NSPanel's pins by default; on any other board from the settings nspPower, nspBuzzer, read at boot
// (nspanelBootSetup). -1: not connected. A pin another part of ESPurna holds already (a relay) is dropped. The display's
// UART is a port of the uart module (nspPort, nspanel_link.cpp): without it the display part stays off
int DisplayPowerPin { 4 };      // LOW = display ON
int BuzzerPin { 21 };

Config running;

} // namespace

namespace build {

void powerWrite(int level) {
    if (DisplayPowerPin >= 0) {
        digitalWrite(DisplayPowerPin, level);
    }
}

} // namespace build

Config& config() {
    return running;
}

// -----------------------------------------------------------------------------
// Screen texts, dates and the decimal separator, by "language" in the JSON
// -----------------------------------------------------------------------------

namespace lang {
namespace {

Language current { Language::English };

const Texts English {
    { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "January", "February", "March", "April", "May", "June",
      "July", "August", "September", "October", "November", "December" },
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
    "On", "Off", "On: ",
    "Settings",
    "Heating", "Target ",
    "Weather: no data", "Today", "Tomorrow", "%ld m/s", "Wind", "Rain", "UV index",
    "Icons", "Large", "Small",
    "Theme", "Light", "Dark",
    "Backdrop", { "Aurora", "Black", "Candy", "Dunes", "Forest" }, // "Background" is wider than the tile name
    "History", "No history yet", " h", "Temperature", "Target", "Humidity",
};

const Texts Russian {
    { "Воскресенье", "Понедельник", "Вторник", "Среда", "Четверг", "Пятница", "Суббота" },
    { "Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб" },
    { "января", "февраля", "марта", "апреля", "мая", "июня",
      "июля", "августа", "сентября", "октября", "ноября", "декабря" },
    { "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек" },
    "Вкл", "Выкл", "Вкл: ",
    "Настройки",
    "Нагрев", "Цель ",
    "Погода: нет данных", "Сегодня", "Завтра", "%ld м/с", "Ветер", "Осадки", "УФ-индекс",
    "Иконки", "Крупные", "Мелкие",
    "Тема", "Светлая", "Тёмная",
    "Фон", { "Сияние", "Чёрный", "Огоньки", "Дюны", "Лес" },
    "История", "Истории пока нет", " ч", "Температура", "Цель", "Влажность",
};

} // namespace

void set(Language language) {
    current = language;
}

const Texts& texts() {
    return (current == Language::Russian) ? Russian : English;
}

const char* weekdayFull(int wday) {
    return texts().weekday_full[((wday % 7) + 7) % 7];
}

const char* weekdayShort(int wday) {
    return texts().weekday_short[((wday % 7) + 7) % 7];
}

// "Saturday, October 24, 2026" / "Суббота, 24 октября 2026", shorter when longer than DateMax
String date(const tm& t) {
    const char* const full = weekdayFull(t.tm_wday);
    const char* const month = texts().month[((t.tm_mon % 12) + 12) % 12];
    const int year = t.tm_year + 1900;
    const bool ru = (current == Language::Russian);

    char buf[96];
    if (ru) {
        snprintf(buf, sizeof(buf), "%s, %d %s %d", full, t.tm_mday, month, year);
    } else {
        snprintf(buf, sizeof(buf), "%s, %s %d, %d", full, month, t.tm_mday, year);
    }
    if (strlen(buf) <= build::DateMax) {
        return buf;
    }

    if (ru) {
        snprintf(buf, sizeof(buf), "%s, %d %s", full, t.tm_mday, month);
    } else {
        snprintf(buf, sizeof(buf), "%s, %s %d", full, month, t.tm_mday);
    }
    if (strlen(buf) <= build::DateMax) {
        return buf;
    }

    if (ru) {
        snprintf(buf, sizeof(buf), "%s, %d %s %d", weekdayShort(t.tm_wday), t.tm_mday, month, year);
    } else {
        snprintf(buf, sizeof(buf), "%s, %s %d, %d", weekdayShort(t.tm_wday), month, t.tm_mday, year);
    }
    return buf;
}

// "Oct 25" / "25 окт"
String dayMonth(int mday, int mon) {
    const char* const month = texts().month_short[((mon % 12) + 12) % 12];
    char buf[24];
    if (current == Language::Russian) {
        snprintf(buf, sizeof(buf), "%d %s", mday, month);
    } else {
        snprintf(buf, sizeof(buf), "%s %d", month, mday);
    }
    return buf;
}

// "Sun, Oct 25" / "Вс, 25 окт" (OHpanel home: 165 px / 26 bytes)
String dateShort(const tm& t) {
    String out(weekdayShort(t.tm_wday));
    out += F(", ");
    out += dayMonth(t.tm_mday, t.tm_mon);
    return out;
}

// a printf'ed number for the screen: "21.5" / "21,5"
void decimal(char* text) {
    if (current != Language::Russian) {
        return;
    }
    for (auto* p = text; *p; ++p) {
        if (*p == '.') {
            *p = ',';
        }
    }
}

} // namespace lang

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
    if ((index < 0) || (static_cast<size_t>(index) >= running.entities.size())) {
        return -1;
    }

    return entityState(running.entities[index]);
}

// -1 when nothing is known, 1 when anything is on
int groupState(int group) {
    if ((group < 0) || (static_cast<size_t>(group) >= running.groups.size())) {
        return -1;
    }

    int out = -1;
    for (const auto entity : running.groups[group]) {
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


// publishes a value to the entity's command topic, through its "payload" template ("%s" = value)
void entitySend(int index, const String& value) {
    if ((index < 0) || (static_cast<size_t>(index) >= running.entities.size())) {
        return;
    }

#if MQTT_SUPPORT
    const auto& entity = running.entities[index];
    if (!entity.command.length()) {
        return;
    }

    if (entity.payload.length() && (entity.payload.indexOf(F("%s")) >= 0)) {
        String payload(entity.payload);
        payload.replace(F("%s"), value);
        mqttSendRaw(entity.command.c_str(), payload.c_str(), false);
    } else {
        mqttSendRaw(entity.command.c_str(), value.c_str(), false);
    }
#endif
}

void entitySet(int index, Action action) {
    if ((index < 0) || (static_cast<size_t>(index) >= running.entities.size())) {
        return;
    }

    const auto& entity = running.entities[index];
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

    // "url": a GET instead of the MQTT command. Toggle goes by the known state, else on. Without a state
    // topic and with two different URLs the panel keeps the state itself, as sent
    if (entity.url_on.length() || entity.url_off.length()) {
        const bool on = (action == Action::On) || ((action == Action::Toggle) && (entity.state != 1));
        const String& url = on ? entity.url_on : entity.url_off;
        if (url.length() && http::get(url) && !entity.topic.length() && (entity.url_on != entity.url_off)) {
            const int state = on ? 1 : 0;
            if (running.entities[index].state != state) {
                running.entities[index].state = state;
                entityChanged(index);
            }
        }
        return;
    }

    const String& on = entity.on.length() ? entity.on : running.on;
    const String& off = entity.off.length() ? entity.off : running.off;
    // unknown state: entities with their own "on" value are switched on, plain ones get mqtt.toggle
    const String* value = entity.on.length() ? &on : &running.toggle;
    switch (action) {
    case Action::On:
        value = &on;
        break;
    case Action::Off:
        value = &off;
        break;
    case Action::Toggle:
        if (entity.state >= 0) {
            value = entity.state ? &off : &on;
        }
        break;
    }

    entitySend(index, *value);
}

void groupSet(int group, Action action) {
    if ((group < 0) || (static_cast<size_t>(group) >= running.groups.size())) {
        return;
    }

    if (action == Action::Toggle) {
        action = (groupState(group) == 1) ? Action::Off : Action::On;
    }

    for (const auto entity : running.groups[group]) {
        entitySet(entity, action);
    }
}

// -----------------------------------------------------------------------------
// Cards: a card is one screen of tiles, a page or a chunk of a page with more tiles than fit
// -----------------------------------------------------------------------------

namespace {

// tiles of a grid page on one screen, the smallest of
// - grids: the driver shows cols x rows (9, 6, 4, 2 or 8): the TFT's page per layout, or drawn tiles;
//   else the layout's columns of the 4x2 slot grid (2 x cols: 6, 6, 4, 2 or 8) and the driver's capacity;
// - "per_screen"
size_t gridCapacity(const Page& page, size_t capacity, bool grids = false) {
    size_t out = grids ? gridTiles(page.grid) : std::min<size_t>(capacity, 2 * gridCols(page.grid));
    if (page.per_screen) {
        out = std::min<size_t>(out, page.per_screen);
    }
    return std::max<size_t>(out, 1);
}

} // namespace

// settings: the driver draws "type": "settings" pages (OHpanel), others skip them
// list_rows: rows per sheet of "type": "list" pages; 0 = the driver shows them as grids
// grids: the driver shows every "layout" in full (gridCapacity())
void buildCards(std::vector<Card>& cards, size_t capacity, size_t title_max, bool settings,
        size_t list_rows, bool grids)
{
    cards.clear();

    // the swipe row (top-level "pages") first, then the sheets of the nested pages (opened by their tiles)
    for (size_t index = 0; index < (2 * running.pages.size()); ++index) {
        const bool sub = (index >= running.pages.size());
        const size_t page = sub ? (index - running.pages.size()) : index;
        const auto& info = running.pages[page];
        if (info.nested != sub) {
            continue;
        }
        if (info.settings) {
            if (settings) {
                Card card;
                card.page = page;
                card.first = 0;
                card.count = SettingsTiles;
                card.title = info.title.length() ? info.title : String(lang::texts().settings);
                card.settings = true;
                card.sub = sub;
                utf8Truncate(card.title, title_max);
                cards.push_back(std::move(card));
            }
            continue;
        }

        const bool list = info.list && list_rows;
        const size_t per = list
            ? std::max<size_t>(1, info.per_screen ? std::min<size_t>(info.per_screen, list_rows) : list_rows)
            : gridCapacity(info, capacity, grids);
        const size_t total = info.items.size();
        const size_t chunks = std::max<size_t>(1, (total + per - 1) / per);

        for (size_t chunk = 0; chunk < chunks; ++chunk) {
            Card card;
            card.page = page;
            card.first = chunk * per;
            card.count = std::min(per, total - std::min(total, chunk * per));
            card.list = list;
            card.sub = sub;
            card.grid = list ? Grid::G4x2 : info.grid;
            card.title = info.title;
            if (chunks > 1) {
                card.title += ' ';
                card.title += String(chunk + 1);
                card.title += '/';
                card.title += String(chunks);
            }
            utf8Truncate(card.title, title_max);
            cards.push_back(std::move(card));
        }
    }
}

// first card of a config page, -1 when there is none
int firstCard(const std::vector<Card>& cards, int page) {
    for (size_t card = 0; card < cards.size(); ++card) {
        if (cards[card].page == page) {
            return card;
        }
    }
    return -1;
}

// cards of the swipe row (they come before the sub-page sheets)
size_t rowCards(const std::vector<Card>& cards) {
    size_t out = 0;
    while ((out < cards.size()) && !cards[out].sub) {
        ++out;
    }
    return out;
}

// both cards exist and are sheets of the same page
bool samePage(const std::vector<Card>& cards, size_t a, size_t b) {
    return (a < cards.size()) && (b < cards.size()) && (cards[a].page == cards[b].page);
}

// sheets of the card's page and the card's position among them
void pageSheets(const std::vector<Card>& cards, size_t card, size_t& count, size_t& current) {
    count = 0;
    current = 0;
    if (card >= cards.size()) {
        return;
    }
    for (size_t n = 0; n < cards.size(); ++n) {
        if (cards[n].page == cards[card].page) {
            if (n == card) {
                current = count;
            }
            ++count;
        }
    }
}

bool itemUses(const Item& item, int entity) {
    if (item.target.entity == entity) {
        return true;
    }

    if ((item.thermostat >= 0) && (static_cast<size_t>(item.thermostat) < running.thermostats.size())) {
        const auto& thermostat = running.thermostats[item.thermostat];
        if ((thermostat.current == entity) || (thermostat.humidity == entity) || (thermostat.active == entity)
            || (thermostat.min == entity) || (thermostat.max == entity))
        {
            return true;
        }
    }

    if (item.target.group >= 0) {
        for (const auto member : running.groups[item.target.group]) {
            if (member == entity) {
                return true;
            }
        }
    }

    return false;
}

bool cardContains(const std::vector<Card>& cards, size_t card, int entity) {
    if (card >= cards.size()) {
        return false;
    }

    const auto& info = cards[card];
    if (info.settings) {
        return false;
    }

    const auto& page = running.pages[info.page];
    for (size_t n = info.first; n < (info.first + info.count); ++n) {
        if (itemUses(running.items[page.items[n]], entity)) {
            return true;
        }
    }

    return false;
}

// number of group members that are on
int groupOnCount(int group) {
    if ((group < 0) || (static_cast<size_t>(group) >= running.groups.size())) {
        return 0;
    }

    int out = 0;
    for (const auto entity : running.groups[group]) {
        if (entityState(entity) == 1) {
            ++out;
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// Room temperature: the NTC on GPIO38 is an ESPurna sensor (NTCSensor), so it shows up in the web UI, the sensor
// MQTT topics and the correction / units settings; the screen shows the first temperature magnitude
// -----------------------------------------------------------------------------

namespace ntc {
namespace {

float reading_value { NAN };
bool reported { false };

#if SENSOR_SUPPORT
// single readings until the first report, then only reports (filtered over snsReport readings)
void update(const espurna::sensor::Value& reading, bool report) {
    if ((reading.type != MAGNITUDE_TEMPERATURE) || (reading.index != 0) || std::isnan(reading.value)) {
        return;
    }

    if (!report && reported) {
        return;
    }

    reported = reported || report;

    // the screen shows Celsius or Fahrenheit as set for the magnitude, never Kelvin
    float next = reading.value;
    if (reading.units == espurna::sensor::Unit::Kelvin) {
        next -= 273.15f;
    }

    if (next != reading_value) {
        reading_value = next;
        driver::temperatureChanged();
    }
}

void onRead(const espurna::sensor::Value& reading) {
    update(reading, false);
}

void onReport(const espurna::sensor::Value& reading) {
    update(reading, true);
}
#endif

void setup() {
#if SENSOR_SUPPORT
    sensorOnMagnitudeRead(onRead);
    sensorOnMagnitudeReport(onReport);
#endif
}

} // namespace

float value() {
    return reading_value;
}

String text() {
    if (!running.temperature || std::isnan(reading_value)) {
        return String();
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0" "C", reading_value);
    return buf;
}

} // namespace ntc

// -----------------------------------------------------------------------------
// Buzzer
// -----------------------------------------------------------------------------

namespace buzzer {
namespace {

bool active { false };
uint32_t until { 0 };

// the chip's square wave (arch/<chip>/buzzer_chip.h)
void duty(uint32_t value) {
    if (BuzzerPin < 0) {
        return;
    }
    espurna::driver::buzzer::duty(BuzzerPin, value);
}

void setup() {
    if (BuzzerPin < 0) {
        return;
    }
    espurna::driver::buzzer::setup(BuzzerPin, 2700);
    duty(0);
}

void loop() {
    if (active && (static_cast<int32_t>(millis() - until) >= 0)) {
        duty(0);
        active = false;
    }
}

} // namespace

void beep() {
    if (!running.beep) {
        return;
    }

    duty(128);
    active = true;
    until = millis() + 40;
}

} // namespace buzzer

// -----------------------------------------------------------------------------
// Tile actions, the same for both drivers. value: -1 toggle (switch), 0 off, 1 on
// -----------------------------------------------------------------------------

void itemTap(size_t index, int value) {
    if (index >= running.items.size()) {
        return;
    }

    const auto& item = running.items[index];
    buzzer::beep();

    switch (item.type) {
    case ItemType::Switch:
        entitySet(item.target.entity,
            (value < 0) ? Action::Toggle : (value ? Action::On : Action::Off));
        break;

    case ItemType::Button:
        entitySet(item.target.entity, Action::On);
        break;

    case ItemType::Scene:
        groupSet(item.target.group, item.target.action);
        break;

    case ItemType::Text:
    case ItemType::Link:       // navigation, handled by the driver
    case ItemType::Thermostat: // opens the thermostat page (OHpanel driver)
        break;
    }
}

// -----------------------------------------------------------------------------
// Thermostat values, the same for both drivers
// -----------------------------------------------------------------------------

float entityNumber(int index) {
    if ((index < 0) || (static_cast<size_t>(index) >= running.entities.size())) {
        return NAN;
    }
    return running.entities[index].number;
}

// heating: "active" is known and not one of the idle words
bool thermostatActive(const Thermostat& thermostat) {
    if ((thermostat.active < 0) || (static_cast<size_t>(thermostat.active) >= running.entities.size())) {
        return false;
    }

    const auto& value = running.entities[thermostat.active].value;
    if (!value.length()) {
        return false;
    }

    String lower(value);
    lower.toLowerCase();
    return (lower != F("off")) && (lower != F("false")) && (lower != F("0")) && (lower != F("idle"));
}

float thermostatMin(const Thermostat& thermostat) {
    const float value = entityNumber(thermostat.min);
    return std::isnan(value) ? thermostat.min_value : value;
}

float thermostatMax(const Thermostat& thermostat) {
    const float value = entityNumber(thermostat.max);
    const float min = thermostatMin(thermostat);
    return (std::isnan(value) || (value <= min)) ? std::max(thermostat.max_value, min + thermostat.step) : value;
}

// snapped to the step grid above min, clamped to min..max
float thermostatClamp(const Thermostat& thermostat, float value) {
    const float min = thermostatMin(thermostat);
    const float max = thermostatMax(thermostat);
    value = min + roundf((value - min) / thermostat.step) * thermostat.step;
    return std::max(min, std::min(max, value));
}

// "21.5" / "21,5" (1 decimal unless the value is whole and the step too)
String temperatureShort(float value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", value);
    lang::decimal(buf);
    return buf;
}

// -----------------------------------------------------------------------------
// On-panel settings (OHpanel settings tiles), kept in ESPurna settings so that they survive a
// reboot and a JSON reload; ohpanel.layout / ohpanel.theme are only the defaults
// -----------------------------------------------------------------------------

void applyUiSettings(Config& out) {
    const auto icons = getSetting(F("nspIcons"));
    if (icons == F("center")) {
        out.layout = Layout::Center;
    } else if (icons == F("corner")) {
        out.layout = Layout::Corner;
    }

    const auto theme = getSetting(F("nspTheme"));
    if (theme == F("light")) {
        out.theme = Theme::Light;
    } else if (theme == F("dark")) {
        out.theme = Theme::Dark;
    }

    const auto background = getSetting(F("nspBg"));
    for (size_t index = 0; index < (sizeof(BackgroundNames) / sizeof(BackgroundNames[0])); ++index) {
        if (background == BackgroundNames[index]) {
            out.background = index;
            break;
        }
    }
}

void saveLayout(Layout layout) {
    running.layout = layout;
    setSetting(F("nspIcons"), String((layout == Layout::Corner) ? F("corner") : F("center")));
}

void saveTheme(Theme theme) {
    running.theme = theme;
    setSetting(F("nspTheme"), String((theme == Theme::Light) ? F("light") : F("dark")));
}

void saveBackground(uint8_t background) {
    running.background = background;
    setSetting(F("nspBg"), String(BackgroundNames[background])); // a bare const char* picks the bool overload
}


// -----------------------------------------------------------------------------
// Config load / reload
// -----------------------------------------------------------------------------

namespace {

String last_load_error;
String last_load_warnings;

} // namespace

void setLoadError(const String& error) {
    last_load_error = error;
}

// a parsed config replaces the running one
void applyConfig(Config&& next, const String& warnings, const char* source) {
    last_load_warnings = warnings;
    if (warnings.length()) {
        DEBUG_MSG_P(PSTR("[NSPANEL] Config warnings: %s\n"), warnings.c_str());
    }

    // keep known MQTT states across reloads (the old config is dropped right after)
    for (auto& entity : next.entities) {
        for (auto& old : running.entities) {
            if (entity.topic.length() && (entity.topic == old.topic) && (entity.path == old.path)
                && (entity.on == old.on) && (entity.off == old.off))
            {
                entity.state = old.state;
                entity.value = std::move(old.value);
                entity.number = old.number;
                break;
            }
        }
    }

    applyUiSettings(next);

    const bool weather_changed = (next.weather != running.weather)
        || (next.lat != running.lat) || (next.lon != running.lon);
    const bool display_changed = (next.display != running.display);

    unsubscribeAll();
    running = std::move(next);
    lang::set(running.language);
    next.entities.clear(); // whatever the move left there, freed before the drivers rebuild
    subscribeAll();

    DEBUG_MSG_P(PSTR("[NSPANEL] Config %s: %u pages, %u items, %u entities, %u groups, heap %u / %u\n"),
        source, running.pages.size(), running.items.size(), running.entities.size(), running.groups.size(),
        systemHeapStats().available, systemHeapStats().usable);

    if (weather_changed) {
        weather::reset();
    }

    if (!link::active()) {
        return; // nspanelSetup() starts the detection
    }

    if (display_changed && !flash::busy()) {
        link::probe("display setting changed");
    } else {
        driver::reload();
    }
}


// Boot and nspanel.reload: /nspanel.json, or the built-in config when there is none. A file that
// cannot be used keeps the running config; only an empty one (boot) falls back to the default.
// Peak: file text + JSON nodes + the new Config (+ the running one on reload), ref. heapAvailable()
void load() {
    String error;
    String warnings;
    const char* source = "(default)";
    bool ok = false;

    ESPURNA_TRY {
        Config next;
        if (storage::exists()) {
            source = build::ConfigPath;
            std::unique_ptr<char[]> text;
            size_t size = 0;
            if (!storage::read(text, size)) {
                error = String(F("cannot read ")) + build::ConfigPath + F(" (empty, too large or no memory)");
            } else {
                ok = (ParseResult::Ok == parseConfig(text.get(), size, next, error, warnings));
            }
        } else {
            ok = (ParseResult::Ok == parseDefault(next, error, warnings));
        }

        if (ok) {
            last_load_error = String();
        } else {
            last_load_error = error;
            DEBUG_MSG_P(PSTR("[NSPANEL] Config error: %s\n"), error.c_str());
            if (!running.pages.empty()) {
                return;
            }

            next = Config();
            warnings = String();
            String ignored;
            if (ParseResult::Ok != parseDefault(next, ignored, warnings)) {
                DEBUG_MSG_P(PSTR("[NSPANEL] Default config failed: %s\n"), ignored.c_str());
                return;
            }
            source = "(default)";
        }

        applyConfig(std::move(next), warnings, source);
    } ESPURNA_CATCH_BAD_ALLOC {
        last_load_error = F("out of memory while applying the config");
        DEBUG_MSG_P(PSTR("[NSPANEL] Config: %s\n"), last_load_error.c_str());
    }
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

namespace {

#if RELAY_SUPPORT
void relayCallback(size_t id, bool) {
    for (size_t index = 0; index < running.entities.size(); ++index) {
        if (running.entities[index].relay == static_cast<int>(id)) {
            entityChanged(index);
        }
    }

    if (auto* active = link::active()) {
        active->relayChanged(id);
    }
}
#endif

#if BUTTON_SUPPORT
void buttonCallback(size_t id, ButtonEvent) {
    if (id >= running.buttons.size()) {
        return;
    }

    const auto& target = running.buttons[id];
    if (!target.valid) {
        return;
    }

    if (target.group >= 0) {
        groupSet(target.group, target.action);
    } else if (target.entity >= 0) {
        entitySet(target.entity, target.action);
    }

    if ((target.page >= 0) && !flash::busy()) {
        driver::showPage(target.page);
    }
}
#endif

// -----------------------------------------------------------------------------
// Terminal
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT

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
        terminalError(ctx, F("nspanel.send <payload> (OHpanel: a Nextion command, FF FF FF is added; Lovelace: a '~' command)"));
        return;
    }

    if (flash::busy()) {
        terminalError(ctx, F("TFT upload running"));
        return;
    }

    String payload;
    for (size_t n = 1; n < ctx.argv.size(); ++n) {
        if (n > 1) {
            payload += ' ';
        }
        payload += ctx.argv[n];
    }

    if (!link::connected()) {
        terminalError(ctx, F("no display driver connected"));
        return;
    }

    driver::send(std::move(payload));
    terminalOK(ctx);
}

void terminalDetect(espurna::terminal::CommandContext&& ctx) {
    if (flash::busy()) {
        terminalError(ctx, F("TFT upload running"));
        return;
    }

    link::probe("terminal");
    terminalOK(ctx);
}

void terminalPage(espurna::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, F("nspanel.page <page index | home | weather | settings>"));
        return;
    }

    if (flash::busy() || !link::connected()) {
        terminalError(ctx, F("display not ready"));
        return;
    }

    const auto& arg = ctx.argv[1];
    int target = PageHome;
    if (arg == F("weather")) {
        target = PageWeather;
    } else if (arg == F("settings")) {
        target = PageSettings;
    } else if (arg != F("home")) {
        target = arg.toInt();
        if ((target < 0) || (static_cast<size_t>(target) >= running.pages.size())) {
            terminalError(ctx, F("no such page"));
            return;
        }
    }

    driver::showPage(target);
    terminalOK(ctx);
}

void terminalInfo(espurna::terminal::CommandContext&& ctx) {
    link::info(ctx.output);
    ctx.output.printf_P(PSTR("Config: %u pages, %u items, %u entities, %u groups, %u subscribed\n"),
        running.pages.size(), running.items.size(), running.entities.size(), running.groups.size(), subscribedCount());
    const auto payloads = json::stats();
    ctx.output.printf_P(PSTR("UI: icons %s, theme %s, %u thermostats, %u info lines; JSON payloads parsed %u, failed %u, dropped %u, pending %u\n"),
        (running.layout == Layout::Corner) ? "corner" : "center", (running.theme == Theme::Light) ? "light" : "dark",
        running.thermostats.size(), running.info_count, payloads.parsed, payloads.failed, payloads.dropped, payloads.pending);
    if (last_load_error.length()) {
        ctx.output.printf_P(PSTR("Config error: %s\n"), last_load_error.c_str());
    }
    if (last_load_warnings.length()) {
        ctx.output.printf_P(PSTR("Config warnings: %s\n"), last_load_warnings.c_str());
    }
    storage::info(ctx.output);
    weather::info(ctx.output);
    ctx.output.printf_P(PSTR("Room temperature: %.1f (%s)\n"), ntc::reading_value,
        ntc::reported ? "sensor report" : std::isnan(ntc::reading_value) ? "no reading yet" : "sensor reading");

    flash::info(ctx.output);

    ctx.output.print(F("Last events:\n"));
    for (const auto& event : panelEvents()) {
        ctx.output.printf_P(PSTR("  %s\n"), event.c_str());
    }

    terminalOK(ctx);
}

// nspanel.flash <http(s)://.../file.tft> [baud]
void terminalFlash(espurna::terminal::CommandContext&& ctx) {
    if ((ctx.argv.size() < 2) || (ctx.argv.size() > 3)) {
        terminalError(ctx, F("nspanel.flash <http(s)://.../nspanel.tft> [baud, default 921600]"));
        return;
    }

    uint32_t rate = 921600;
    if (ctx.argv.size() == 3) {
        static constexpr uint32_t Rates[] { 115200, 230400, 250000, 256000, 512000, 921600 };
        rate = strtoul(ctx.argv[2].c_str(), nullptr, 10);
        bool known = false;
        for (const auto value : Rates) {
            known = known || (value == rate);
        }
        if (!known) {
            terminalError(ctx, F("baud: 115200, 230400, 250000, 256000, 512000 or 921600"));
            return;
        }
    }

    String error;
    if (!flash::start(ctx.argv[1], rate, error)) {
        terminalError(ctx, error);
        return;
    }

    ctx.output.print(F("TFT upload started, progress in the log and in nspanel.info. Do not power off the panel.\n"));
    terminalOK(ctx);
}

// nspanel.capture start | print | stop: records the commands sent to the OHpanel TFT (start also sends the
// theme again, so a capture replays on its own in the Nextion Editor simulator)
void terminalCapture(espurna::terminal::CommandContext&& ctx) {
    const String arg = (ctx.argv.size() == 2) ? ctx.argv[1] : String();
    auto* active = link::active();
    if (!active || !active->capture(Driver::Capture::Probe, ctx.output)) {
        terminalError(ctx, F("OHpanel driver only"));
        return;
    }
    if (arg == F("start")) {
        active->capture(Driver::Capture::Start, ctx.output);
    } else if (arg == F("stop")) {
        active->capture(Driver::Capture::Stop, ctx.output);
    } else if (arg == F("print")) {
        active->capture(Driver::Capture::Dump, ctx.output);
    } else {
        terminalError(ctx, F("nspanel.capture start | print | stop"));
        return;
    }
    terminalOK(ctx);
}

// nspanel.event <page> <component> <value>: as if the OHpanel TFT sent it (a tap without a finger)
void terminalEvent(espurna::terminal::CommandContext&& ctx) {
    auto* active = link::active();
    if ((ctx.argv.size() != 4) || !active
        || !active->event(ctx.argv[1].c_str(), ctx.argv[2].c_str(), ctx.argv[3].c_str()))
    {
        terminalError(ctx, F("nspanel.event <page> <component> <value> (OHpanel driver only)"));
        return;
    }
    terminalOK(ctx);
}

// nspanel.url <http(s)://...>: the GET a "url" tile sends, to try an address from the panel
void terminalUrl(espurna::terminal::CommandContext&& ctx) {
    if ((ctx.argv.size() != 2)
        || !(ctx.argv[1].startsWith(F("http://")) || ctx.argv[1].startsWith(F("https://"))))
    {
        terminalError(ctx, F("nspanel.url <http(s)://...>, the result follows in the log"));
        return;
    }
    if (!http::get(ctx.argv[1])) {
        terminalError(ctx, F("not started (no WiFi, busy or low heap)"));
        return;
    }
    terminalOK(ctx);
}

PROGMEM_STRING(PanelCapture, "NSPANEL.CAPTURE");
PROGMEM_STRING(PanelEvent, "NSPANEL.EVENT");
PROGMEM_STRING(PanelReload, "NSPANEL.RELOAD");
PROGMEM_STRING(PanelSend, "NSPANEL.SEND");
PROGMEM_STRING(PanelInfo, "NSPANEL.INFO");
PROGMEM_STRING(PanelFlash, "NSPANEL.FLASH");
PROGMEM_STRING(PanelDetect, "NSPANEL.DETECT");
PROGMEM_STRING(PanelPage, "NSPANEL.PAGE");
PROGMEM_STRING(PanelUrl, "NSPANEL.URL");

static constexpr espurna::terminal::Command PanelCommands[] PROGMEM {
    {PanelReload, terminalReload},
    {PanelSend, terminalSend},
    {PanelInfo, terminalInfo},
    {PanelFlash, terminalFlash},
    {PanelDetect, terminalDetect},
    {PanelPage, terminalPage},
    {PanelCapture, terminalCapture},
    {PanelEvent, terminalEvent},
    {PanelUrl, terminalUrl},
};

#endif // TERMINAL_SUPPORT

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void panelLoop() {
    upload::process();
    netLoop();

    // the upload task owns UART2; frames queued meanwhile are dropped on resume
    if (flash::busy()) {
        flash::loop();
        buzzer::loop();
        return;
    }

    link::loop();

    weather::loop();
    http::loop();
    buzzer::loop();
}

} // namespace
} // namespace nspanel
} // namespace espurna

void nspanelBootSetup() {
    using namespace espurna::nspanel;

    // a pin of the settings: valid and not held by anything else yet, else -1
    const auto pin = [](const char* key, int fallback) -> int {
        const auto value = getSetting(espurna::settings::Key(key), fallback);
        if ((value < 0) || !gpioValid(static_cast<unsigned char>(value))
            || !gpioLock(static_cast<unsigned char>(value)))
        {
            if (value >= 0) {
                DEBUG_MSG_P(PSTR("[NSPANEL] %s GPIO%d is not free, not used\n"), key, value);
            }
            return -1;
        }
        return value;
    };

    DisplayPowerPin = pin("nspPower", DisplayPowerPin);
    BuzzerPin = pin("nspBuzzer", BuzzerPin);

    if (DisplayPowerPin >= 0) {
        pinMode(DisplayPowerPin, OUTPUT);
        build::powerWrite(LOW);
    }

    if (BuzzerPin >= 0) {
        pinMode(BuzzerPin, OUTPUT);
        digitalWrite(BuzzerPin, LOW);
    }
}

void nspanelSetup() {
    using namespace espurna::nspanel;

    // the display's port of the uart module; without it there is no display to drive
    if (!uart::begin()) {
        DEBUG_MSG_P(PSTR("[NSPANEL] No UART port for the display (nspPort, uartTx<n> / uartRx<n>), the display part is off\n"));
        return;
    }

#if NTC_SUPPORT
    gpioLock(NTC_PIN);
#endif

    ntc::setup();
    buzzer::setup();

    storage::mount();
    load();

    setupNet();
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

    link::probe("boot");
    espurnaRegisterLoop(panelLoop);
}

#endif // NSPANEL_SUPPORT
