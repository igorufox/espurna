/*

NEXTION MODULE - link

The UART port of the display (a port of the uart module, nspPort; the NSPanel's TX GPIO16, RX GPIO17 by default), the
display detection that picks the Driver: OHpanel (nextion_ohpanel.cpp) or Lovelace (nextion_lovelace.cpp), and the
module's setup: the display of the tile pages of the screen module (setDisplay(), screen_pages.h) and a panel of the
screen (nextion_screen.cpp)

*/

#include "espurna.h"
#include "libs/PrintString.h"

#if NEXTION_SUPPORT

#include "nextion.h"
#include "screen.h"
#include "screen_pages.h"
#include "uart.h"

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#include "json.h"
#include "gpio.h"
#include "pins.h"

#if WEB_SUPPORT
#include "ws.h"
#endif

namespace espurna {
namespace pages {

// -----------------------------------------------------------------------------
// UART2 and state shared by the display drivers
// -----------------------------------------------------------------------------

namespace uart {
namespace {

// the display's UART: a port of the uart module (uartTx<n> / uartRx<n>, the same on every chip), nspPort is n + 1.
// The older keys nspTx / nspRx (or the NSPanel's own pins 16 / 17) become such a port once: those pins, 115200, a
// 2 KB receive buffer (the TFT upload and the answers of the drivers)
constexpr size_t PortsMax { 4 };
constexpr size_t RxBuffer { 2048 };

size_t index { 0 };
::espurna::ByteStream* stream { nullptr };
uint32_t current { 0 };

// the port of the older pin keys: the one with these pins, else the first without pins
size_t migrate() {
    const auto tx = getSetting("nspTx", 16);
    const auto rx = getSetting("nspRx", 17);

    size_t out = PortsMax;
    for (size_t n = 0; n < PortsMax; ++n) {
        const auto port_tx = getSetting({"uartTx", n}, -1);
        const auto port_rx = getSetting({"uartRx", n}, -1);
        if ((port_tx == tx) && (port_rx == rx)) {
            out = n;
            break;
        }
        if ((out == PortsMax) && (port_tx < 0) && (port_rx < 0)) {
            out = n;
        }
    }

    if (out == PortsMax) {
        return out;
    }

    setSetting({"uartTx", out}, tx);
    setSetting({"uartRx", out}, rx);
    setSetting({"uartBaud", out}, build::LovelaceBaud);
    setSetting({"uartRxBuf", out}, RxBuffer);
    setSetting("nspPort", out + 1);
    delSetting("nspTx");
    delSetting("nspRx");
    DEBUG_MSG_P(PSTR("[NEXTION] The display is on UART port #%u now (TX %d, RX %d)\n"),
        static_cast<unsigned>(out), tx, rx);
    return out;
}

} // namespace

::espurna::ByteStream& port() {
    return *stream;
}

bool begin() {
    const auto number = getSetting("nspPort", 0);
    index = (number > 0) ? static_cast<size_t>(number - 1) : migrate();
    if (index >= PortsMax) {
        return false;
    }

    auto opened = uartOpen(index);
    if (!opened || !opened->tx || !opened->rx || !uartClaim(index, "NSPanel")) {
        return false;
    }

    stream = opened->stream;
    current = getSetting({"uartBaud", index}, build::LovelaceBaud);
    setBaud(build::LovelaceBaud);
    return true;
}

void drainInput() {
    while (port().read() >= 0) {
    }
}

// waits for what is queued only while switching, at most ~1 KB at 115200
void rate(uint32_t value) {
    if (value != current) {
        uartBaud(index, value);
        current = value;
    }
}

void setBaud(uint32_t value) {
    rate(value);
    drainInput();
}

int writable() {
    constexpr int Command { 256 };
    const auto out = port().availableForWrite();
    return (out > 0) ? out : Command;
}

// raw Nextion command, terminated by FF FF FF
void writeRaw(const char* command) {
    auto& serial = port();
    serial.write(reinterpret_cast<const uint8_t*>(command), strlen(command));
    const uint8_t end[3] {0xFF, 0xFF, 0xFF};
    serial.write(end, sizeof(end));
}

} // namespace uart

// -----------------------------------------------------------------------------
// Display detection. Exactly one driver owns UART2 at a time.
//
// auto: OHpanel at 921600 (FF FF FF sendme + get design_ver, expects 66 / 71) for 600 ms, then
// Lovelace at 115200 (raw recmod=1, pageType~pageStartup, expects event,startup) for 2 s, repeated
// until one answers. Forced: only that driver, retried. No answer for 30 s: the display is
// power-cycled once (GPIO4 true 800 ms, 1.5 s boot).
// Detection runs again at boot, after every TFT upload, when "display" changes, on
// `nspanel.detect`, and when the active driver reports the TFT lost.
// The end of a TFT upload does what an ESP restart does (W78): the display gets FlashSettleMs to
// finish writing and restart by itself, then its power is cut and given back (the reset of the ESP
// leaves GPIO4 floating, the display off, at every boot), the port goes back to the detection rate
// and the detection starts as at boot. Without this the display stayed silent after the upload
// (rx 0) until the ESP restarted.
// -----------------------------------------------------------------------------

namespace link {
namespace {

enum class Power {
    None,
    Settle,     // after a TFT upload: the display finishes on its own power
    Off,
    Boot,
};

Driver* current { nullptr };
Driver* detected { nullptr };
bool is_probing { true };
uint32_t window_start { 0 };
uint32_t episode_start { 0 };
bool power_cycled { false };
Power power { Power::None };
uint32_t power_at { 0 };
uint32_t probes { 0 };
espurna::string reason { "boot" };

const char* nameOf(const Driver* driver) {
    return driver ? driver->name() : "none";
}

Driver& first() {
    switch (config().display) {
    case DriverPref::Lovelace:
        return lovelace::driver();
    case DriverPref::Ohpanel:
    case DriverPref::Auto:
        break;
    }
    return ohpanel::driver();
}

Driver& next(const Driver* now) {
    switch (config().display) {
    case DriverPref::Lovelace:
        return lovelace::driver();
    case DriverPref::Ohpanel:
        return ohpanel::driver();
    case DriverPref::Auto:
        break;
    }
    return (now == &ohpanel::driver()) ? lovelace::driver() : ohpanel::driver();
}

uint32_t window(const Driver* driver) {
    const bool forced = (config().display != DriverPref::Auto);
    if (driver == &lovelace::driver()) {
        return forced ? build::ProbeForcedLovelaceMs : build::ProbeLovelaceMs;
    }
    return forced ? build::ProbeForcedOhpanelMs : build::ProbeOhpanelMs;
}

void start(Driver& driver) {
    current = &driver;
    window_start = espurna::time::ms();
    ++probes;
    driver.begin();
}

} // namespace

Driver* active() {
    return current;
}

const char* name() {
    return nameOf(current);
}

bool connected() {
    return current && current->connected();
}

bool probing() {
    return is_probing;
}

void probe(const char* why) {
    DEBUG_MSG_P(PSTR("[NEXTION] Detecting the display (%s, display: %s)\n"), why, driverPrefName(config().display));
    reason = why;
    is_probing = true;
    episode_start = espurna::time::ms();
    power_cycled = false;
    power = Power::None;
    build::powerWrite(false);
    start(first());
}

// as at boot: the display power-cycled, the port at the detection rate, then detected
// (no driver until the display is back: the one before the upload starts over in Power::Boot)
void afterFlash(bool done) {
    reason = done ? "TFT uploaded" : "TFT upload failed";
    DEBUG_MSG_P(PSTR("[NEXTION] Detecting the display (%s, display: %s) after a power cycle\n"),
        reason.c_str(), driverPrefName(config().display));
    current = nullptr;
    is_probing = true;
    episode_start = espurna::time::ms();
    power_cycled = false;
    uart::setBaud(build::LovelaceBaud);
    build::powerWrite(false);
    power = Power::Settle;
    power_at = espurna::time::ms();
}

void loop() {
    const auto now = espurna::time::ms();

    switch (power) {
    case Power::Settle:
        if (now - power_at >= build::FlashSettleMs) {
            DEBUG_MSG_P(PSTR("[NEXTION] Power-cycling the display after the TFT upload\n"));
            build::powerWrite(true);
            power = Power::Off;
            power_at = now;
        }
        return;

    case Power::Off:
        if (now - power_at >= build::PowerOffMs) {
            build::powerWrite(false);
            power = Power::Boot;
            power_at = now;
        }
        return;

    case Power::Boot:
        if (now - power_at >= build::PowerBootMs) {
            power = Power::None;
            start(first());
        }
        return;

    case Power::None:
        break;
    }

    if (current) {
        current->loop();
    }

    if (!is_probing) {
        if (!current || current->lost()) {
            probe("display lost");
        }
        return;
    }

    if (connected()) {
        is_probing = false;
        detected = current;
        DEBUG_MSG_P(PSTR("[NEXTION] Display driver: %s (after %u ms, %u probes)\n"),
            nameOf(current), static_cast<unsigned>(now - episode_start), probes);
        return;
    }

    if (current && current->found()) {
        return; // identifying
    }

    if (now - window_start < window(current)) {
        return;
    }

    if (!power_cycled && (now - episode_start >= build::ProbePowerCycleMs)) {
        DEBUG_MSG_P(PSTR("[NEXTION] No display answer for %u s, power-cycling it\n"),
            static_cast<unsigned>((now - episode_start) / 1000));
        power_cycled = true;
        power = Power::Off;
        power_at = now;
        build::powerWrite(true);
        return;
    }

    start(next(current));
}

void info(::espurna::Output& out) {
    out.printf_P(PSTR("Display: driver %s (setting \"%s\"), %s, version '%s', model '%s'\n"),
        nameOf(current), driverPrefName(config().display),
        flash::busy() ? "TFT upload running"
            : connected() ? "connected"
            : is_probing ? "detecting" : "not connected",
        driver::version(), driver::model());
    out.printf_P(PSTR("Detect: last '%s', %u probes, %u s ago, last found %s\n"),
        reason.c_str(), probes,
        static_cast<unsigned>((espurna::time::ms() - episode_start) / 1000), nameOf(detected));
    if (current) {
        current->info(out);
    }
}

} // namespace link

// -----------------------------------------------------------------------------
// The display of the pages: this link (screen_pages.h Display)
// -----------------------------------------------------------------------------

namespace {

int DisplayPowerPin { 4 };      // false = display ON (the NSPanel's GPIO4, nspPower)

class NextionDisplay final : public Display {
public:
    const char* name() const override {
        return "nextion";
    }

    Driver* active() override {
        return link::active();
    }

    bool busy() const override {
        return flash::busy();
    }

    bool probing() const override {
        return link::probing();
    }

    void probe(const char* why) override {
        link::probe(why);
    }

    void loop() override {
        // the upload task owns the UART; frames queued meanwhile are dropped on resume
        if (flash::busy()) {
            flash::loop();
            return;
        }

        link::loop();
    }

    void info(::espurna::Output& out) override {
        link::info(out);
        flash::info(out);
    }

    void flashInfo(::espurna::Output& out) override {
        flash::info(out);
    }

    // the NSPanel's buzzer
    int buzzerPin() const override {
        return 21;
    }
};

NextionDisplay nextion_display;

} // namespace

namespace build {

void powerWrite(int level) {
    if (DisplayPowerPin >= 0) {
        espurna::pins::write(DisplayPowerPin, level);
    }
}

} // namespace build

namespace link {

Display& display() {
    return nextion_display;
}

// the NSPanel's display power pin (nspPower), early: also in the unstable mode
void bootSetup() {
    const auto value = getSetting("nspPower", DisplayPowerPin);
    if ((value < 0) || !gpioValid(static_cast<unsigned char>(value)) || !gpioLock(static_cast<unsigned char>(value))) {
        if (value >= 0) {
            DEBUG_MSG_P(PSTR("[NEXTION] nspPower GPIO%d is not free, not used\n"), value);
        }
        DisplayPowerPin = -1;
        return;
    }

    DisplayPowerPin = value;
    espurna::pins::mode(DisplayPowerPin, espurna::pins::Output);
    build::powerWrite(false);
}

} // namespace link

// -----------------------------------------------------------------------------
// The TFT upload: the terminal, the WebUI
// -----------------------------------------------------------------------------

namespace {

#if TERMINAL_SUPPORT

// nspanel.flash <http(s)://.../file.tft> [baud]
void terminalFlash(espurna::terminal::CommandContext&& ctx) {
    if ((ctx.argv.size() < 2) || (ctx.argv.size() > 3)) {
        terminalError(ctx, "nspanel.flash <http(s)://.../nspanel.tft> [baud, default 921600 | check]");
        return;
    }

    // nspanel.flash <url> check: the download alone, its CRC-32 in the log, the display left alone
    if ((ctx.argv.size() == 3) && (ctx.argv[2] == "check")) {
        espurna::string error;
        if (!flash::check(ctx.argv[1], error)) {
            terminalError(ctx, error);
            return;
        }
        ctx.output.print("TFT download check started, the CRC-32 follows in the log and in nspanel.info\n");
        terminalOK(ctx);
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
            terminalError(ctx, "baud: 115200, 230400, 250000, 256000, 512000 or 921600");
            return;
        }
    }

    espurna::string error;
    if (!flash::start(ctx.argv[1], rate, error)) {
        terminalError(ctx, error);
        return;
    }

    ctx.output.print("TFT upload started, progress in the log and in nspanel.info. Do not power off the panel.\n");
    terminalOK(ctx);
}


PROGMEM_STRING(PanelFlash, "NSPANEL.FLASH");

static constexpr espurna::terminal::Command FlashCommands[] PROGMEM {
    {PanelFlash, terminalFlash},
};

#endif // TERMINAL_SUPPORT

#if WEB_SUPPORT

// "Flash the screen": the TFT by its URL, as nspanel.flash at its default rate; any screen, also the stock one
void onWsAction(uint32_t client_id, const char* action, espurna::json::Value data) {
    if (strcmp_P(action, PSTR("nspanelFlash")) != 0) {
        return;
    }

    espurna::string out;
    const auto url = data["url"].asString();
    if (flash::start(espurna::string(url.c_str()), 921600, out)) {
        out = "started";
    }
    wsPost(client_id, [out](espurna::json::Writer& root) {
        root.field("nspanelFlash", out);
    });
}

// shows the NEXTION section of the WebUI
void onWsVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, STRING_VIEW("nextion"));
}

#endif // WEB_SUPPORT

} // namespace

} // namespace pages
} // namespace espurna

void nextionBootSetup() {
    espurna::pages::link::bootSetup();
}

void nextionSetup() {
    using namespace espurna::pages;

    // the display's port of the uart module; without it there is no display to drive
    if (!uart::begin()) {
        DEBUG_MSG_P(PSTR("[NEXTION] No UART port for the display (nspPort, uartTx<n> / uartRx<n>), off\n"));
        return;
    }

#if NTC_SUPPORT
    // the NSPanel's thermistor (an ESPurna sensor of its own; the pages show the first temperature)
    gpioLock(NTC_PIN);
#endif

#if TERMINAL_SUPPORT
    espurna::terminal::add(FlashCommands);
#endif
#if WEB_SUPPORT
    wsRegister()
        .onVisible(onWsVisible)
        .onAction(onWsAction);
#endif

    // the pages of the screen module on it, and the screen's QR page
    setDisplay(&link::display());
    nextionScreenSetup();
}

#endif // NEXTION_SUPPORT
