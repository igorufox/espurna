/*

NSPANEL MODULE - link

UART2 (TX GPIO16, RX GPIO17 by default) ownership, the display events, and the display detection
that picks the Driver: OHpanel (nspanel_ohpanel.cpp) or Lovelace (nspanel_lovelace.cpp)

*/

#include "espurna.h"
#include "libs/PrintString.h"

#if NSPANEL_SUPPORT

#include "nspanel_core.h"
#include "uart.h"

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#include "json.h"

namespace espurna {
namespace nspanel {

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
    DEBUG_MSG_P(PSTR("[NSPANEL] The display is on UART port #%u now (TX %d, RX %d)\n"),
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

namespace {

struct Panel {
    std::deque<espurna::string> events;
    espurna::string info; // last driver announcement, republished on MQTT connect
};

Panel panel;

void recordEvent(const espurna::string& message) {
    if (panel.events.size() >= build::EventsMax) {
        panel.events.pop_front();
    }
    panel.events.push_back(message);
}

} // namespace

// every TFT event: kept for nspanel.info and published to <root>/nspanel
void publishEvent(const espurna::string& message) {
    recordEvent(message);
#if MQTT_SUPPORT
    if (mqttConnected()) {
        mqttSendRaw(mqttTopic("nspanel").c_str(), message.c_str(), false);
    }
#endif
}

// {"driver":"ohpanel","version":"bc629e1e","model":"..."} to <root>/nspanel
void announceDriver(const Driver& driver, const espurna::string& version, const espurna::string& model) {
    StringPrint out;
    {
        espurna::json::StreamWriter root(out);
        root.beginObject();
        root.field(STRING_VIEW("driver"), driver.name());
        root.field(STRING_VIEW("version"), version);
        root.field(STRING_VIEW("model"), model);
        root.end();
    }
    panel.info = std::move(static_cast<espurna::string&>(out));
    publishEvent(panel.info);
}

const espurna::string& panelInfo() {
    return panel.info;
}

const std::deque<espurna::string>& panelEvents() {
    return panel.events;
}

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
    DEBUG_MSG_P(PSTR("[NSPANEL] Detecting the display (%s, display: %s)\n"), why, driverPrefName(config().display));
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
    DEBUG_MSG_P(PSTR("[NSPANEL] Detecting the display (%s, display: %s) after a power cycle\n"),
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
            DEBUG_MSG_P(PSTR("[NSPANEL] Power-cycling the display after the TFT upload\n"));
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
        DEBUG_MSG_P(PSTR("[NSPANEL] Display driver: %s (after %u ms, %u probes)\n"),
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
        DEBUG_MSG_P(PSTR("[NSPANEL] No display answer for %u s, power-cycling it\n"),
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
// The active driver
// -----------------------------------------------------------------------------

void entityChanged(int entity) {
    if (auto* active = link::active()) {
        active->entityChanged(entity);
    }
}

namespace driver {

void weatherChanged() {
    if (auto* active = link::active()) {
        active->weatherChanged();
    }
}

void temperatureChanged() {
    if (auto* active = link::active()) {
        active->temperatureChanged();
    }
}

void reload() {
    if (auto* active = link::active()) {
        active->reload();
    }
}

void showPage(int page) {
    if (auto* active = link::active()) {
        active->showPage(page);
    }
}

void send(espurna::string payload) {
    if (auto* active = link::active()) {
        active->send(std::move(payload));
    }
}

const char* version() {
    return link::connected() ? link::active()->version() : "";
}

const char* model() {
    return link::connected() ? link::active()->model() : "";
}

// OHpanel: the design's name and what it has (grid layouts, backgrounds)
espurna::string design() {
    return link::connected() ? link::active()->design() : espurna::string();
}

} // namespace driver

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_SUPPORT
