/*

NSPANEL MODULE - what the parts of the module share

nspanel.cpp          config lifecycle, entities, cards, screen texts, room temperature, buzzer, terminal, setup
nspanel_config.cpp   config model and parser (nspanel_config.h), host tested
nspanel_storage.cpp  the config in the flash data of the module, parse heap budget
nspanel_net.cpp      MQTT subscriptions and state payloads, history series, weather, "url" requests
nspanel_link.cpp     UART2, display events, display detection, the active Driver
nspanel_flash.cpp    Nextion TFT upload
nspanel_web.cpp      config upload hand-over, /nspanel endpoints, WebUI screen mirror actions
nspanel_lovelace.cpp joBr99 Lovelace UI TFT driver
nspanel_ohpanel.cpp  OHpanel TFT driver

Everything one part needs from another is declared here as a function; the state stays in its file.
Not for the rest of the firmware (that is nspanel.h)

*/

#pragma once

#include "nspanel_config.h"

#include <HardwareSerial.h>

#include <deque>
#include <memory>
#include <vector>

#include <ctime>

namespace espurna {
namespace nspanel {

namespace build {

constexpr uint32_t LovelaceBaud { 115200 };
constexpr uint32_t OhpanelBaud { 921600 };

constexpr size_t EventsMax { 8 };

// display detection (link::)
constexpr uint32_t ProbeOhpanelMs { 600 };      // auto: one `sendme` + `get design_ver` at 921600
constexpr uint32_t ProbeLovelaceMs { 2000 };    // auto: pageType~pageStartup at 115200, wait for event,startup
constexpr uint32_t ProbeForcedOhpanelMs { 2000 };
constexpr uint32_t ProbeForcedLovelaceMs { 5000 };
constexpr uint32_t ProbePowerCycleMs { 30000 }; // no answer for this long: power-cycle the display once
constexpr uint32_t PowerOffMs { 800 };
constexpr uint32_t PowerBootMs { 1500 };
constexpr uint32_t FlashQuietMs { 180000 };     // no automatic power-cycle this long after a TFT upload

constexpr char ConfigPath[] = "/nspanel.json";
constexpr char ConfigTmpPath[] = "/nspanel.json.tmp";

// config parse heap budget, ref. heapAvailable(): the Config built from the text takes about
// 3 bytes per byte of JSON; this much stays free for WiFi / lwIP / a TLS session meanwhile
constexpr size_t ConfigHeapPerByte { 3 };
constexpr size_t ConfigHeapReserve { 16384 };
constexpr size_t ConfigBlockReserve { 1024 };

// POST /nspanel.json: loop() waits this long for a running weather fetch (TLS, 40-60 KB)
// before parsing; the client gets 503 when no answer is ready after UploadAnswerMs
constexpr uint32_t UploadWeatherWaitMs { 10000 };
constexpr uint32_t UploadAnswerMs { 15000 };

constexpr size_t WeatherHeapMin { 50000 };
constexpr uint32_t WeatherRetryMs { 60000 };
constexpr size_t WeatherBodyMax { 8192 };      // the 7 day forecast is ~1.5 KB

// MQTT entity subscriptions are sent from loop(), a few at a time (ref. subscribeLoop())
constexpr size_t SubscribeBurst { 2 };
constexpr uint32_t SubscribeStepMs { 20 };

// display power (nspPower, LOW = display ON), nothing when the pin is not connected
void powerWrite(int level);

} // namespace build

// -----------------------------------------------------------------------------
// Screen texts, dates and the decimal separator, by "language" in the JSON (nspanel.cpp)
// -----------------------------------------------------------------------------

namespace lang {

struct Texts {
    const char* weekday_full[7];
    const char* weekday_short[7];
    const char* month[12];          // as used in a date: "October" / "октября"
    const char* month_short[12];
    const char* on;
    const char* off;
    const char* on_count;           // followed by the number of devices on
    const char* settings;
    const char* heating;
    const char* target;             // followed by the setpoint
    const char* no_weather;
    const char* today;
    const char* tomorrow;
    const char* wind_speed;         // printf, one %ld (m/s)
    const char* wind;
    const char* rain;
    const char* uv;
    const char* icons;
    const char* icons_large;
    const char* icons_small;
    const char* theme;
    const char* theme_light;
    const char* theme_dark;
    const char* background;
    const char* backgrounds[5];     // same order as BackgroundNames
    const char* history;            // thermostat: the chart button
    const char* no_history;
    const char* hours;              // chart title suffix, followed by the number of hours
    const char* temperature;        // chart legend
    const char* setpoint;
    const char* humidity;
};

void set(Language language);
const Texts& texts();
const char* weekdayFull(int wday);
const char* weekdayShort(int wday);

// "Saturday, October 24, 2026" / "Суббота, 24 октября 2026", shorter when longer than DateMax
String date(const tm& t);

// "Oct 25" / "25 окт"
String dayMonth(int mday, int mon);

// "Sun, Oct 25" / "Вс, 25 окт"
String dateShort(const tm& t);

// a printf'ed number for the screen: "21.5" / "21,5"
void decimal(char* text);

} // namespace lang

// -----------------------------------------------------------------------------
// Running config and entities (nspanel.cpp)
// -----------------------------------------------------------------------------

Config& config();

// a parsed config replaces the running one
void applyConfig(Config&& next, const String& warnings, const char* source);

// boot and nspanel.reload: /nspanel.json, or the built-in config when there is none
void load();

// the last load / upload error, empty when the running config is the file's
void setLoadError(const String& error);

int entityState(const Entity& entity);
int entityState(int index);

// -1 when nothing is known, 1 when anything is on
int groupState(int group);

// number of group members that are on
int groupOnCount(int group);

// publishes a value to the entity's command topic, through its "payload" template ("%s" = value)
void entitySend(int index, const String& value);
void entitySet(int index, Action action);
void groupSet(int group, Action action);

// the entity's state or value changed: the active display driver redraws what shows it
void entityChanged(int entity);

// tile actions, the same for both drivers. value: -1 toggle (switch), 0 off, 1 on
void itemTap(size_t index, int value);

bool itemUses(const Item& item, int entity);

// thermostat values, the same for both drivers
float entityNumber(int index);
bool thermostatActive(const Thermostat& thermostat);
float thermostatMin(const Thermostat& thermostat);
float thermostatMax(const Thermostat& thermostat);
float thermostatClamp(const Thermostat& thermostat, float value);

// "21.5" / "21,5"
String temperatureShort(float value);

// on-panel settings (OHpanel settings tiles), kept in ESPurna settings
void applyUiSettings(Config& out);
void saveLayout(Layout layout);
void saveTheme(Theme theme);
void saveBackground(uint8_t background);

// -----------------------------------------------------------------------------
// Cards (nspanel.cpp): a card is one screen of tiles, a page or a chunk of a page with more tiles than fit
// -----------------------------------------------------------------------------

struct Card {
    uint16_t page;
    uint16_t first;
    uint16_t count;
    String title;
    bool settings { false }; // the settings tiles (count of them), no items
    bool list { false };     // rows of a "type": "list" page (only when the driver draws lists)
    bool sub { false };      // a sheet of a nested page: after all cards of the swipe row
    Grid grid { Grid::G4x2 };// the page's "layout"
};

constexpr size_t SettingsTiles { 3 };

// settings: the driver draws "type": "settings" pages (OHpanel), others skip them
// list_rows: rows per sheet of "type": "list" pages; 0 = the driver shows them as grids
// grids: the driver shows every "layout" in full
void buildCards(std::vector<Card>& cards, size_t capacity, size_t title_max, bool settings = false,
    size_t list_rows = 0, bool grids = false);

// first card of a config page, -1 when there is none
int firstCard(const std::vector<Card>& cards, int page);

// cards of the swipe row (they come before the sub-page sheets)
size_t rowCards(const std::vector<Card>& cards);

// both cards exist and are sheets of the same page
bool samePage(const std::vector<Card>& cards, size_t a, size_t b);

// sheets of the card's page and the card's position among them
void pageSheets(const std::vector<Card>& cards, size_t card, size_t& count, size_t& current);

bool cardContains(const std::vector<Card>& cards, size_t card, int entity);

// -----------------------------------------------------------------------------
// Room temperature, buzzer (nspanel.cpp)
// -----------------------------------------------------------------------------

namespace ntc {

// the first temperature magnitude of the ESPurna sensors, NAN until there is one
float value();

// "21.5°C", empty when there is none or the config hides it
String text();

} // namespace ntc

namespace buzzer {

void beep();

} // namespace buzzer

// -----------------------------------------------------------------------------
// Network (nspanel_net.cpp)
// -----------------------------------------------------------------------------

// MQTT entity subscriptions, sent from loop() a few at a time
void unsubscribeAll();
void subscribeAll();
size_t subscribedCount();
void setupNet();
void netLoop();

namespace json {

struct Stats {
    uint32_t parsed;
    uint32_t failed;
    uint32_t dropped;
    size_t pending;
};

Stats stats();

} // namespace json

// a retained history series, subscribed only while its chart is open
namespace history {

constexpr size_t PayloadMax { 2048 };   // MQTT_BUFFER_MAX_SIZE of the NSPanel build
constexpr size_t PointsMax { 192 };
constexpr int16_t Missing { INT16_MIN };

struct Series {
    uint32_t from { 0 };
    uint32_t step { 0 };
    std::vector<int16_t> temperature;
    std::vector<int16_t> setpoint;
    std::vector<int16_t> humidity;
    std::vector<int16_t> on;    // a switch: % of each step it was ON
};

void open(const String& name);
void close();

// a new message arrived and was not taken yet
bool fresh();

// a new message, parsed into out; false when there is none or it is not a series
bool take(Series& out);

} // namespace history

// Open-Meteo, fetched in a task of its own
namespace weather {

constexpr size_t DaysMax { 7 };

struct Day {
    int code { -1 };
    float tmax { 0.0f };
    float tmin { 0.0f };
    int wday { 0 };
    int mday { 1 };
    int mon { 0 };         // 0..11
    float wind { NAN };    // m/s, daily max
    int rain { -1 };       // precipitation probability max, %
    float uv { NAN };      // UV index max
};

struct Data {
    bool valid { false };
    float temperature { 0.0f };
    int code { -1 };
    bool day { true };
    Day days[DaysMax];
    size_t count { 0 };
    time_t updated { 0 };
};

const Data& current();

// the location changed: forget the data, fetch again
void reset();

// the fetch task holds a TLS session, config parsing waits for it
bool running();

void info(Print& out);

// starts the fetch task when it is time, takes its result
void loop();

} // namespace weather

// "url" tiles: an HTTP GET per tap, in a task of its own
namespace http {

// false when it could not be started (no WiFi, too many at once, low heap)
bool get(const String& url);

// logs the result of the last request
void loop();

} // namespace http

// -----------------------------------------------------------------------------
// Storage (nspanel_storage.cpp)
// -----------------------------------------------------------------------------

namespace storage {

bool mount();
bool mounted();

// the config file is there (mounted or not)
bool exists();

bool read(String& out);

// NUL terminated copy of the file for an in-place parse, exactly size + 1 bytes
bool read(std::unique_ptr<char[]>& out, size_t& out_size);

void removeTmp();
bool writeTmp(const char* data, size_t size);
bool commitTmp();

void info(Print& out);

} // namespace storage

// built-in config text
const char* defaultConfig();

// what the parse checks against the rest of the firmware (relays, MQTT buffer)
Limits parseLimits();

// Free heap a parse needs: the JSON nodes plus the Config being built plus what WiFi / lwIP / TLS must keep
bool heapAvailable(size_t json_size, size_t length, String& error);

// heap check + parse, for text the caller does not need afterwards
ParseResult parseConfig(char* text, size_t length, Config& out, String& error, String& warnings);

// built-in config, parsed from a heap copy (in place, like the file)
ParseResult parseDefault(Config& out, String& error, String& warnings);

// -----------------------------------------------------------------------------
// Display drivers (nspanel_lovelace.cpp, nspanel_ohpanel.cpp). Exactly one owns UART2 at a time
// -----------------------------------------------------------------------------

class Driver {
public:
    enum class Capture {
        Probe, // only whether the driver has it
        Start,
        Stop,
        Dump,
    };

    virtual const char* name() const = 0;

    // take UART2 and look for the TFT
    virtual void begin() = 0;
    virtual void loop() = 0;

    virtual bool connected() const = 0;

    // something answered: link:: keeps this driver while it identifies the TFT
    virtual bool found() const {
        return false;
    }

    // the TFT stopped answering or something else talks on the line: link:: detects again
    virtual bool lost() const = 0;

    // only while connected
    virtual const char* version() const = 0;
    virtual const char* model() const = 0;

    // OHpanel: the design's name and what it has (grid layouts, backgrounds)
    virtual String design() const {
        return String();
    }

    virtual void entityChanged(int entity) = 0;
    virtual void relayChanged(size_t) {
    }
    virtual void weatherChanged() = 0;
    virtual void temperatureChanged() = 0;
    virtual void reload() = 0;
    virtual void showPage(int page) = 0;
    virtual void send(String payload) = 0;
    virtual void info(Print& out) = 0;

    // the WebUI screen mirror, false when the driver has none
    virtual bool mirrorStart() {
        return false;
    }

    // a TFT event as if the TFT sent it (WebUI mirror, nspanel.event), false when the driver has none
    virtual bool event(const char*, const char*, const char*) {
        return false;
    }

    // a touch by coordinates (WebUI mirror)
    virtual void touchXY(int, int, bool) {
    }

    // nspanel.capture: the commands sent to the TFT, false when the driver has none
    virtual bool capture(Capture, Print&) {
        return false;
    }

protected:
    // one static object per driver, never deleted through this
    ~Driver() = default;
};

namespace lovelace {

Driver& driver();

} // namespace lovelace

namespace ohpanel {

Driver& driver();

} // namespace ohpanel

// -----------------------------------------------------------------------------
// UART2, display events, detection (nspanel_link.cpp)
// -----------------------------------------------------------------------------

namespace uart {

// the display's port, a port of the uart module (nspPort); begin() false - no port, the display part stays off
Stream& port();
bool begin();
void drainInput();

// bytes the port takes without waiting (a core that does not tell: one Nextion command at a time)
int writable();

// another speed: rate() alone, setBaud() also drops what came in at the old one (probe)
void rate(uint32_t value);
void setBaud(uint32_t value);

// raw Nextion command, terminated by FF FF FF
void writeRaw(const char* command);

} // namespace uart

// every TFT event: kept for nspanel.info and published to <root>/nspanel
void publishEvent(const String& message);

// {"driver":"ohpanel","version":"bc629e1e","model":"..."} to <root>/nspanel
void announceDriver(const Driver& driver, const String& version, const String& model);

// the last driver announcement, republished on MQTT connect
const String& panelInfo();

const std::deque<String>& panelEvents();

namespace link {

// the driver owning UART2 (also while it probes), nullptr before the first probe
Driver* active();

// name of the active driver, "none" without one
const char* name();

bool connected();
bool probing();

// detect the display again
void probe(const char* why);
void afterFlash(bool done);

void loop();

// Display: and Detect: lines of nspanel.info, then the driver's own
void info(Print& out);

} // namespace link

// the active driver, nothing without one
namespace driver {

void weatherChanged();
void temperatureChanged();
void reload();
void showPage(int page);
void send(String payload);

// only while connected, "" otherwise
const char* version();
const char* model();
String design();

} // namespace driver

// -----------------------------------------------------------------------------
// Nextion TFT upload (nspanel_flash.cpp)
// -----------------------------------------------------------------------------

namespace flash {

bool busy();
bool start(const String& link, uint32_t rate, String& out);
void loop();
void info(Print& out);

} // namespace flash

// -----------------------------------------------------------------------------
// Config upload and web (nspanel_web.cpp)
// -----------------------------------------------------------------------------

namespace upload {

// a config is on its way: no weather TLS session (40-60 KB) is started meanwhile
bool busy();

// loop(): validate, save and apply a received config
void process();

} // namespace upload

void setupWeb();

} // namespace nspanel
} // namespace espurna
