/*

MATTER MODULE

The one Matter layer of both chips (ref. espurna_matter.h): settings, the start, the relay link both ways, the bridged
plug names, controllers (fabrics), the pairing window and codes, the quiet mode while a controller commissions, the
log of the Matter task, the terminal commands and the MATTER panel of the WebUI. The Matter stack is reached only
through the Matter application C API (`matter_app_*`), the same on the ESP32 and on Beken

*/

#include "espurna.h"

#if MATTER_SUPPORT

#include "espurna_matter.h"
#include "matter_identity.h"
#include "espurna_ota.h"
#include "espurna_tasks.h"
#include "relay.h"
#include "terminal.h"
#include "ws.h"

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#if ALEXA_SUPPORT
#include "alexa.h"
#endif

#if defined(MATTER_UDP_LOG)
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace espurna {
namespace matter {
namespace {

bool started { false };
bool connected { false };

// The automatic start (`matterAuto`, on by default) waits `matterDelay` seconds after the station got its address, by default the
// backend's start_delay(). After a restart by the network watchdog the start waits at least 5 minutes.
constexpr uint32_t NetworkResetStartDelay { 300 };

// espurna::time::ms() | 1 of the automatic start that is due, 0 when none
uint32_t start_at { 0 };

// Plug endpoints of the relays: as many as the data model of the chip's Matter application has, at most 4
constexpr size_t MaxPlugs { 4 };

// Filled in the loop by start() for the Matter task (matter_node_label() ...), constant while Matter runs. Matter
// limits the node label, product name and the bridged names to 32 characters
struct PlugName {
    char label[33];
    char unique[33];
};

uint8_t plug_count { 0 };
PlugName plug_names[MaxPlugs];
char node_label[33];
char serial_number[33];
char product_label[33];
uint16_t device_type { 0 };

// What the Matter task reports (matter_onoff_changed() ...), applied in the loop in the same order: a relay switched
// on and off again by a controller ends off, a commissioning that started and ended ends the quiet mode
struct Event {
    enum class Type : uint8_t {
        OnOff,
        Commissioning,
        Window,
        Pong,
    };

    Type type;
    uint8_t endpoint;
    bool value;
};

// a controller switching every plug at once, plus the commissioning and window events around it
constexpr size_t EventsMax { 16 };

espurna::system::EventQueue<Event, EventsMax> events;

// The Matter task waits this long for the loop to make room, a relay change is not to be lost
constexpr auto EventWait = duration::Milliseconds{ 50 };

// Commissioning in progress, for the Matter task itself (only that task reads and writes it): the message trace of the
// log only goes out then
bool commissioning_active { false };

// The pairing window is open (the Matter task): the message trace goes out too, a controller that tries and never
// gets to the PASE step shows in the log (2026-11-14: the speaker hub "did not even start", the log had nothing)
bool window_open { false };

// A controller that keeps opening CASE with a fabric the device does not have (it thinks the device is still its own:
// a pairing that did not complete, the device reset meanwhile): five lines a try, a few tries a second. One line a
// minute says what it is instead (the Matter task)
struct UnknownCase {
    uint32_t last { 0 };
    uint32_t reported { 0 };
    uint32_t count { 0 };
};

UnknownCase unknown_case;

constexpr uint32_t UnknownCaseReportMs { 60000 };
constexpr uint32_t UnknownCaseFloodMs { 10000 };

// Fabrics a device keeps (CHIP_CONFIG_MAX_FABRICS is 5 by default)
constexpr size_t MaxControllers { 8 };

// Pairing window, applied in the loop. The stack does not tell how long a window stays open: the basic window (button,
// first boot) closes after 15 minutes, a window a controller opened for another one ends earlier with its close event
uint32_t window_since { 0 };
constexpr uint32_t WindowMs { 15 * 60 * 1000 };

// seconds left of the open pairing window, 0 when it is closed
uint32_t window_left() {
    if (!window_since) {
        return 0;
    }

    const auto elapsed = espurna::time::ms() - window_since;
    if (elapsed >= WindowMs) {
        window_since = 0;
        return 0;
    }

    return std::max<uint32_t>(1, (WindowMs - elapsed) / 1000);
}

void copy_text(char* out, size_t size, const char* text, size_t length) {
    if (!size) {
        return;
    }

    const auto count = std::min(size - 1, length);
    std::memcpy(out, text, count);
    out[count] = '\0';
}

void copy_text(char* out, size_t size, const espurna::string& text) {
    copy_text(out, size, text.c_str(), text.length());
}

void web_update();

// Log lines of the Matter task, printed from the loop (ESPurna debug output is not for other tasks).
// Fixed slots, a line that does not fit is counted as dropped
namespace log {

constexpr size_t Slots { 8 };
constexpr size_t Length { 120 };

char lines[Slots][Length];
size_t head { 0 };
size_t tail { 0 };
size_t dropped { 0 };
espurna::system::Mutex mutex;

void push(const char* line) {
    // A burst of lines waits a little for the loop to print them (it runs every 10 ms)
    for (size_t attempt = 0; attempt < 5; ++attempt) {
        {
            espurna::system::MutexLock lock(mutex);
            if ((head - tail) < Slots) {
                std::strncpy(lines[head % Slots], line, Length - 1);
                lines[head % Slots][Length - 1] = '\0';
                ++head;
                return;
            }
        }
        espurna::time::sleep(10);
    }

    espurna::system::MutexLock lock(mutex);
    ++dropped;
}

void print() {
    char line[Length];
    for (;;) {
        size_t lost { 0 };
        bool empty;

        {
            espurna::system::MutexLock lock(mutex);
            empty = (head == tail);
            if (!empty) {
                std::memcpy(line, lines[tail % Slots], Length);
                ++tail;
            }
            lost = dropped;
            dropped = 0;
        }

        if (lost) {
            DEBUG_MSG_P(PSTR("[MATTER] (%u lines dropped)\n"), lost);
        }

        if (empty) {
            break;
        }

        DEBUG_MSG_P(PSTR("[MATTER] %s\n"), line);
    }
}

} // namespace log

#if defined(MATTER_UDP_LOG)
// Bring-up aid for boards without a serial wire, off unless built with -DMATTER_UDP_LOG="a.b.c.d": every Matter log
// line and start step as one UDP packet to MATTER_UDP_LOG (a.b.c.d), port 5514, sent from the tcpip thread, so it
// works while the loop is stuck
namespace udplog {

struct Message {
    size_t length;
    char text[160];
};

udp_pcb* pcb { nullptr };

void send_tcpip(void* arg) {
    auto* message = static_cast<Message*>(arg);
    if (!pcb) {
        pcb = udp_new();
    }

    if (pcb) {
        auto* buffer = pbuf_alloc(PBUF_TRANSPORT, message->length, PBUF_RAM);
        if (buffer) {
            std::memcpy(buffer->payload, message->text, message->length);
            ip_addr_t address;
            ipaddr_aton(MATTER_UDP_LOG, &address);
            udp_sendto(pcb, buffer, &address, 5514);
            pbuf_free(buffer);
        }
    }

    free(message);
}

void send(const char* text) {
    auto* message = static_cast<Message*>(malloc(sizeof(Message)));
    if (!message) {
        return;
    }

    message->length = snprintf(message->text, sizeof(message->text), "%u %u %s",
        static_cast<unsigned>(espurna::time::ms()), static_cast<unsigned>(systemFreeHeap()), text);
    message->length = std::min(message->length, sizeof(message->text) - 1);
    if (tcpip_callback(send_tcpip, message) != ERR_OK) {
        free(message);
    }
}

} // namespace udplog
#define MATTER_STEP(X) udplog::send(X)
#else
#define MATTER_STEP(X)
#endif

void web_tick();

// Quiet mode while a controller commissions: the certificate checks (PASE / CASE, CSR, NOC) need tens of KB of heap
// at once, more than is left with every module busy. MQTT disconnects, the Alexa servers stop; settings stay, everything
// comes back at the end (or after QuietMaxMs without an end event). The WebUI stays connected (closing it showed
// "Connection lost" in the middle of a pairing), only its periodic status and the debug log are held back (wsPause()):
// a 4-relay Beken bridge ran out of heap in the status update while the hub paired it
constexpr uint32_t QuietMaxMs { 5 * 60 * 1000 };
uint32_t quiet_since { 0 };

#if MQTT_SUPPORT
bool quiet_mqtt { false };
#endif

void quiet(bool active) {
    if (active == (quiet_since != 0)) {
        return;
    }

    quiet_since = active ? (espurna::time::ms() | 1) : 0;

#if WEB_SUPPORT
    wsPause(active);
#endif

#if MQTT_SUPPORT
    if (active) {
        quiet_mqtt = mqttEnabled();
        if (quiet_mqtt) {
            mqttEnabled(false);
            mqttDisconnect();
        }
    } else if (quiet_mqtt) {
        // MQTT switched off in the settings meanwhile stays off
        mqttEnabled(getSetting("mqttEnabled", 1 == MQTT_ENABLED));
        quiet_mqtt = false;
    }
#endif

#if ALEXA_SUPPORT
    alexaPause(active);
#endif

    DEBUG_MSG_P(PSTR("[MATTER] Quiet mode %s, heap %u\n"), active ? "on" : "off", systemFreeHeap());
}

void start();
int fabrics();

// controllers when the last commissioning started, -1 when none is going on (the loop)
int fabrics_before { -1 };

// The verdict on a commissioning that ended, this long after its end: the fabric table the loop reads is a copy, taken
// only when the CHIP stack lock is free, and right at the end the Matter task still holds it (2026-11-15: "did not
// complete" logged a second before "Commissioning complete", the second controller was there 4 s later)
constexpr uint32_t VerdictMs { 10000 };
uint32_t verdict_at { 0 };

void verdict() {
    if (!verdict_at || ((espurna::time::ms() - verdict_at) < VerdictMs)) {
        return;
    }

    verdict_at = 0;
    if (fabrics_before < 0) {
        return;
    }

    if (fabrics() <= fabrics_before) {
        DEBUG_MSG_P(PSTR("[MATTER] Pairing did not complete, no controller was added: if the controller's app "
            "lists the device, remove it there and pair again\n"));
    } else {
        DEBUG_MSG_P(PSTR("[MATTER] Pairing complete, controllers: %d\n"), fabrics());
    }
    fabrics_before = -1;
}

void loop() {
    if (start_at && connected && (static_cast<int32_t>(espurna::time::ms() - start_at) >= 0) && !pendingDeferredReset()
#if OTA_WEB_SUPPORT
        && !otaWebUploading()
#endif
    ) {
        start_at = 0;
        start();
    }

    verdict();

    const auto lost = events.dropped();
    if (lost) {
        DEBUG_MSG_P(PSTR("[MATTER] %u events of the Matter task lost\n"), lost);
    }

    bool commissioning { false };

    Event event;
    while (events.pop(event)) {
        switch (event.type) {
        case Event::Type::OnOff:
            if (event.endpoint <= plug_count) {
                relayStatus(event.endpoint - 1, event.value);
            }
            break;

        case Event::Type::Commissioning:
            commissioning = true;
            DEBUG_MSG_P(PSTR("[MATTER] Commissioning %s\n"), event.value ? "started" : "ended");
            quiet(event.value);
            // a commissioning that ends without a new controller: the fail-safe ran out before the controller's
            // CommissioningComplete and the stack rolled its fabric back. The controller may still list the device
            if (event.value) {
                fabrics_before = fabrics();
                verdict_at = 0;
            } else if (fabrics_before >= 0) {
                verdict_at = espurna::time::ms() | 1;
            }
            break;

        case Event::Type::Window:
            window_since = event.value ? (espurna::time::ms() | 1) : 0;
            DEBUG_MSG_P(PSTR("[MATTER] Pairing window %s\n"), event.value ? "open" : "closed");
            web_update();
            break;

        case Event::Type::Pong:
            DEBUG_MSG_P(PSTR("[MATTER] Task is alive\n"));
            break;
        }
    }

    if (!commissioning && quiet_since && ((espurna::time::ms() - quiet_since) > QuietMaxMs)) {
        DEBUG_MSG_P(PSTR("[MATTER] No end of commissioning in %u s, resuming\n"), QuietMaxMs / 1000);
        quiet(false);
    }

    log::print();
    web_tick();
}

// ---------------------------------------------------------------------------------------------------------------------
// The Matter application through its C API

void plug_name(size_t index, PlugName& out) {
    auto name = getSetting({"relayName", index}, espurna::string());
    if (!name.length()) {
        name = systemHostname();
        name += ' ';
        name += espurna::string(index + 1);
    }

    copy_text(out.label, sizeof(out.label), name);
    snprintf(out.unique, sizeof(out.unique), "%s-%u", systemChipId().toString().c_str(), static_cast<unsigned>(index));
}

// Heap checkpoints (W88): here in the loop "start", "prepare" (the chip's storage, Beken factory data) and "chip stack"
// (matter_app_start(): InitChipStack, MemoryInit, the Matter task and its event queue; the server init is only
// scheduled). The rest come from the Matter task, in the order of the library's InitServer(): "identity" and
// "identity done" (matter_identity_install()), "server init" (matter_plug_count(): Server::Init - fabric table, sessions,
// access control, the data model, mDNS, the pairing window - and the network commissioning cluster), "ota"
// (matter_node_label(): plugs enabled, OTA requestor), "ready" (the fabric watch work item: node label, station mode)
bool start_stack() {
    MATTER_STEP("start");
    heap_step("start", true);

    if (!backend::prepare()) {
        DEBUG_MSG_P(PSTR("[MATTER] Storage or identity of the chip failed\n"));
        return false;
    }

    heap_step("prepare", true);

    device_type = backend::device_type();
    if (device_type) {
        DEBUG_MSG_P(PSTR("[MATTER] Device type 0x%04X of the certification declaration\n"), device_type);
    }

    copy_text(node_label, sizeof(node_label), systemHostname());
    // Basic Information SerialNumber: the speaker hub reads it while commissioning and stops when it is missing. The
    // chip id (MAC) is unique and stays across updates
    copy_text(serial_number, sizeof(serial_number), systemChipId().toString());
    copy_text(product_label, sizeof(product_label), systemDevice().toString());

    const size_t library = matter_app_plugs ? matter_app_plugs() : 1;
    plug_count = static_cast<uint8_t>(std::min({relayCount(), library, MaxPlugs}));
    for (size_t index = 0; index < plug_count; ++index) {
        plug_name(index, plug_names[index]);
    }

    MATTER_STEP("ready, matter_app_start");
    const auto out = matter_app_start() == 0;
    MATTER_STEP(out ? "matter_app_start OK" : "matter_app_start FAILED");
    heap_step("chip stack", true);
    if (out) {
        matter_watch_fabrics();
    }

    return out;
}

void set_onoff(size_t index, bool on) {
    if (started && (index < plug_count)) {
        matter_set_onoff(static_cast<uint8_t>(index + 1), on);
    }
}

int fabrics() {
    return started ? matter_fabric_count() : 0;
}

// Controllers the device was added to (fabrics): for the WebUI list, and to remove one that the controller app forgot
// (deleting a device in the hub's phone app does not tell the device; adding it to the same app again then fails)
struct Controller {
    uint8_t index;
    uint16_t vendor;
    uint64_t node;
    char label[33];
};

size_t controllers(Controller* out, size_t size) {
    if (!started || !matter_fabric_info) {
        return 0;
    }

    size_t count { 0 };
    while (count < size) {
        auto& entry = out[count];
        if (!matter_fabric_info(count, &entry.index, &entry.vendor, &entry.node, entry.label, sizeof(entry.label))) {
            break;
        }
        ++count;
    }

    return count;
}

bool remove_controller(uint8_t index) {
    if (!started || !matter_remove_fabric) {
        return false;
    }

    const auto out = matter_remove_fabric(index);
    if (out) {
        DEBUG_MSG_P(PSTR("[MATTER] Controller #%u removed\n"), index);
    }

    return out;
}

void open_window() {
    if (started) {
        matter_open_window();
    }
}

bool codes(char* manual, size_t manual_size, char* qr, size_t qr_size) {
    return started && matter_onboarding_codes(manual, manual_size, qr, qr_size);
}

// "vendor (0xVID) / product (0xPID), identity" for the panel and the status command
espurna::string product() {
    backend::Identity identity;
    backend::describe(identity);

    espurna::string out;
    out.reserve(112);
    out += identity.vendor;
    out += " (0x";
    out += espurna::string(identity.vendor_id, 16);
    out += ") / ";
    out += identity.product;
    out += " (0x";
    out += espurna::string(identity.product_id, 16);
    out += ')';
    if (identity.source.length()) {
        out += ", ";
        out += identity.source;
    }

    return out;
}

void start() {
    if (started) {
        return;
    }

    started = start_stack();
    DEBUG_MSG_P(PSTR("[MATTER] Start %s, %u endpoint(s), heap %u\n"),
        started ? "OK" : "FAILED", plug_count, systemFreeHeap());

    if (started) {
        for (size_t index = 0; index < plug_count; ++index) {
            set_onoff(index, relayStatus(index) == RelayStatus::On);
        }
    }
}

void wifi_event(espurna::wifi::Event event) {
    switch (event) {
    case espurna::wifi::Event::StationConnected:
        connected = true;
        backend::network(true);
        // the matter module switched on = Matter runs; `matterAuto 0` keeps it for MATTER.START. A crash loop is the
        // module guard's (the modules go off with a reason)
        if (getSetting("matterAuto", true) && !started && !start_at) {
            auto delay = getSetting("matterDelay", backend::start_delay());
            if (customResetReason() == CustomResetReason::Network) {
                delay = std::max(delay, NetworkResetStartDelay);
            }

            if (!delay) {
                start();
            } else {
                DEBUG_MSG_P(PSTR("[MATTER] Starting in %u s\n"), delay);
                start_at = (espurna::time::ms() + (delay * 1000)) | 1;
            }
        }
        break;

    case espurna::wifi::Event::StationDisconnected:
        if (connected) {
            connected = false;
            backend::network(false);
        }
        break;

    default:
        break;
    }
}

#if TERMINAL_SUPPORT
PROGMEM_STRING(Status, "MATTER");

void command_status(::terminal::CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("started: %s, fabrics: %d, window: %s, pairing: %s, heap: %u, lowest since boot: %u\n"),
        started ? "yes" : "no", fabrics(), window_open ? "open" : "closed", commissioning_active ? "yes" : "no",
        systemFreeHeap(), systemHeapMinimum());

    Controller list[MaxControllers];
    const auto count = controllers(list, MaxControllers);
    for (size_t index = 0; index < count; ++index) {
        ctx.output.printf_P(PSTR("controller #%u vendor 0x%04X node 0x%08X%08X label \"%s\"\n"),
            list[index].index, list[index].vendor,
            static_cast<uint32_t>(list[index].node >> 32), static_cast<uint32_t>(list[index].node),
            list[index].label);
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Remove, "MATTER.REMOVE");

// MATTER.REMOVE <#>: the device leaves that controller (the number of the MATTER list)
void command_remove(::terminal::CommandContext&& ctx) {
    if (!started) {
        terminalError(ctx, "Matter is not started");
        return;
    }

    if (ctx.argv.size() != 2) {
        terminalError(ctx, "MATTER.REMOVE <controller #>");
        return;
    }

    const auto index = std::strtoul(ctx.argv[1].c_str(), nullptr, 10);
    if (!index || (index > 254) || !remove_controller(static_cast<uint8_t>(index))) {
        terminalError(ctx, "No such controller");
        return;
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Window, "MATTER.WINDOW");

// Commissioning window again: it closes after 15 minutes and after a controller added the device
void command_window(::terminal::CommandContext&& ctx) {
    if (!started) {
        terminalError(ctx, "Matter is not started");
        return;
    }

    open_window();
    terminalOK(ctx);
}

PROGMEM_STRING(Ping, "MATTER.PING");

// Work item through the Matter task, answered from the loop
void command_ping(::terminal::CommandContext&& ctx) {
    if (!started) {
        terminalError(ctx, "Matter is not started");
        return;
    }

    matter_ping();
    terminalOK(ctx);
}

PROGMEM_STRING(Start, "MATTER.START");

void command_start(::terminal::CommandContext&& ctx) {
    if (!connected) {
        terminalError(ctx, "Not connected to Wi-Fi");
        return;
    }

    start();
    if (!started) {
        terminalError(ctx, "Start failed");
        return;
    }

    terminalOK(ctx);
}

// The manual pairing code and the QR payload carry the setup passcode, they are for the device owner only
PROGMEM_STRING(Codes, "MATTER.CODES");

void command_codes(::terminal::CommandContext&& ctx) {
    if (!started) {
        terminalError(ctx, "Matter is not started");
        return;
    }

    char manual[24];
    char qr[32];
    if (!codes(manual, sizeof(manual), qr, sizeof(qr))) {
        terminalError(ctx, "No commissioning data");
        return;
    }

    ctx.output.printf_P(PSTR("manual pairing code: %s\nQR code: %s\n"), manual, qr);
    terminalOK(ctx);
}

PROGMEM_STRING(Factory, "MATTER.FACTORY");

// Where the device attestation comes from. Public part only, no keys, passcode or verifier
void command_factory(::terminal::CommandContext&& ctx) {
    backend::Identity identity;
    backend::describe(identity);

    ctx.output.printf_P(PSTR("identity: %s\n"), identity.source.c_str());
    ctx.output.printf_P(PSTR("vendor: %s (0x%04X), product: %s (0x%04X)\n"),
        identity.vendor.c_str(), identity.vendor_id, identity.product.c_str(), identity.product_id);
    if (identity.details.length()) {
        ctx.output.printf_P(PSTR("%s\n"), identity.details.c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Reset, "MATTER.RESET");

// Forget every controller (fabrics, ACL), keep the own identity and its pairing code. Not while Matter runs: it
// holds all of that in memory and writes it back
void command_reset(::terminal::CommandContext&& ctx) {
    if (started) {
        terminalError(ctx, "Matter is running, reboot first (matterAuto 0 keeps it off)");
        return;
    }

    if (!backend::reset()) {
        terminalError(ctx, "Matter storage failed");
        return;
    }

    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Status, command_status},
    {Codes, command_codes},
    {Start, command_start},
    {Ping, command_ping},
    {Window, command_window},
    {Remove, command_remove},
    {Factory, command_factory},
    {Reset, command_reset},
};
#endif

// ---------------------------------------------------------------------------------------------------------------------
// MATTER panel of the WebUI: status, start, auto start and the pairing codes (they carry the setup passcode, the panel
// is behind the WebUI login like the rest of the settings)

#if WEB_SUPPORT
namespace web {

STRING_VIEW_INLINE(Prefix, "matter");

int fabrics { -1 };
espurna::time::CoreClock::time_point last;

bool onKeyCheck(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(Prefix);
}

void onVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, Prefix);
}

void onData(espurna::json::Writer& root) {
    root.field("matterStarted", started ? "yes" : "no");
    root.field("matterFabrics", matter::fabrics());
    root.field("matterProduct", product());
    root.field("matterWindow", started ? window_left() : 0);
    // the pairing needs ~15 KB of heap: the panel warns below it (W93)
    root.field("matterHeap", systemFreeHeap());
    root.field("matterHeapMin", systemHeapMinimum());
    // [[#, vendor id, "node id (hex)", "label"], ...]
    root.array("matterControllers");
    Controller entries[MaxControllers];
    const auto count = controllers(entries, MaxControllers);
    for (size_t index = 0; index < count; ++index) {
        char node[17];
        snprintf_P(node, sizeof(node), PSTR("%08X%08X"),
            static_cast<uint32_t>(entries[index].node >> 32), static_cast<uint32_t>(entries[index].node));

        root.beginArray();
        root.value(entries[index].index);
        root.value(entries[index].vendor);
        root.value(node);
        root.value(entries[index].label);
        root.end();
    }
    root.end();

    char manual[24] {};
    char qr[32] {};
    if (codes(manual, sizeof(manual), qr, sizeof(qr))) {
        root.field("matterManual", manual);
        root.field("matterQr", qr);
    } else {
        root.field("matterManual", "");
        root.field("matterQr", "");
    }
}

// The identity choice (`matterIdentity`): in use, what the board has. No secrets: the custom VID / PID only
void onIdentity(espurna::json::Writer& root) {
    root.field("matterIdUse", identity::name(identity::choice()));
    root.field("matterIdFactory", identity::available(identity::Choice::Factory));
    uint16_t vendor { 0 };
    uint16_t product { 0 };
    if (identity::custom_ids(vendor, product)) {
        char text[24];
        snprintf_P(text, sizeof(text), PSTR("VID 0x%04X PID 0x%04X"), vendor, product);
        root.field("matterIdCustom", text);
    } else {
        root.field("matterIdCustom", "");
    }
}

void onConnected(espurna::json::Writer& root) {
    root.field("matterAuto", getSetting("matterAuto", true));
    onIdentity(root);
    onData(root);
}

void message(uint32_t client_id, espurna::string text) {
    wsPost(client_id, [text](espurna::json::Writer& root) {
        root.field("message", text);
    });
}

// Another identity: the controllers verified the old DAC. The panel asked for the confirmation; the restart, and the
// module setup forgets the controllers before Matter starts (identity::setup(), backend::reset() as MATTER.RESET)
void use_identity(uint32_t client_id, identity::Choice choice) {
    setSetting(identity::Setting, espurna::string(identity::name(choice)));
    message(client_id, "Matter identity changed, restarting. Add the device to the controllers again.");
    prepareReset(CustomResetReason::Web);
}

// matterIdentity {use}: factory / espurna / custom
void action_identity(uint32_t client_id, espurna::json::Value data) {
    identity::Choice choice;
    const auto use = data["use"].asString();
    if (!identity::parse(use.c_str(), choice) || !identity::available(choice)) {
        message(client_id, "This Matter identity is not available on this board");
        return;
    }

    if (choice == identity::choice()) {
        setSetting(identity::Setting, espurna::string(identity::name(choice)));
        message(client_id, "This Matter identity is in use already");
        return;
    }

    use_identity(client_id, choice);
}

// matterIdentityUpload {dac, key, pai, cd}: base64 of DER, checked and stored, then in use (restart). The key text of
// the message is wiped after the check, it is never logged or sent back
void action_identity_upload(uint32_t client_id, espurna::json::Value data) {
    // base64 has no escapes: the texts as they are in the message
    auto key = data["key"].asString();
    const auto dac = data["dac"].asString();
    const auto pai = data["pai"].asString();
    const auto cd = data["cd"].asString();

    espurna::string error;
    if (commissioning_active) {
        error = "A controller is adding the device now, try again after it";
    } else {
        // two signature checks and a signature: seconds on the BK7231N, the loop watchdog gets the commissioning time
        backend::commissioning(true);
        identity::custom_store(dac.c_str(), const_cast<char*>(key.c_str()), pai.c_str(),
            cd.c_str(), error);
        backend::commissioning(commissioning_active);
    }

    // the key text: the copy and the message it came in
    if (key.size()) {
        std::memset(const_cast<char*>(key.c_str()), 0, key.size());
    }
    const auto raw = data["key"].view();
    if (raw.length()) {
        std::memset(const_cast<char*>(raw.data()), 0, raw.length());
    }

    if (error.length()) {
        message(client_id, error);
        return;
    }

    use_identity(client_id, identity::Choice::Custom);
}

void onAction(uint32_t client_id, const char* action, espurna::json::Value data) {
    if (std::strcmp(action, "matterIdentity") == 0) {
        action_identity(client_id, data);
        return;
    }

    if (std::strcmp(action, "matterIdentityUpload") == 0) {
        action_identity_upload(client_id, data);
        return;
    }

    if (std::strcmp(action, "matterRemove") == 0) {
        const auto index = data["index"].asLong();
        if ((index > 0) && (index < 255)) {
            remove_controller(static_cast<uint8_t>(index));
            // the list goes out again from web_tick() when the fabric count changed
        }
        return;
    }

    if (std::strcmp(action, "matterWindow") == 0) {
        open_window();
        // the button is grey until an answer: the state now, the countdown follows with the open event
        wsPost(onData);
        return;
    }

    if (std::strcmp(action, "matterStart") != 0) {
        return;
    }

    if (connected) {
        start();
    }

    wsPost(onData);
}

void setup() {
    wsRegister()
        .onVisible(onVisible)
        .onConnected(onConnected)
        .onAction(onAction)
        .onKeyCheck(onKeyCheck);
}

} // namespace web

// a controller was added or removed: refresh the panel
void web_tick() {
    if (!started) {
        return;
    }

    const auto now = espurna::time::CoreClock::now();
    if ((now - web::last) < espurna::duration::Seconds(2)) {
        return;
    }
    web::last = now;

    const auto count = fabrics();
    if (count != web::fabrics) {
        web::fabrics = count;
        if (wsConnected()) {
            wsPost(web::onData);
        }
    }
}

// pairing window opened or closed
void web_update() {
    if (wsConnected()) {
        wsPost(web::onData);
    }
}
#else
void web_tick() {
}

void web_update() {
}
#endif

void setup() {
    // left by older builds (the Matter code in the OTA partition): nothing reads it any more
    delSetting("matterSkip");

    backend::setup();
    // `matterIdentity`, before any start: another identity than at the last start forgets the controllers
    identity::setup();
#if TERMINAL_SUPPORT
    terminal::add(Commands);
#endif
#if WEB_SUPPORT
    web::setup();
#endif
    wifiRegister(wifi_event);
    relayOnStatusChange(set_onoff);
    espurnaRegisterLoop(loop);
    // no restart of ESPurna (Apply, settings, an update) while a controller commissions: it would roll the new fabric
    // back with the controller already holding the device
    systemOnResetHold([]() { return commissioning_active; });
}

} // namespace

void heap_step(const char* step, bool from_loop) {
    const auto stats = systemHeapStats();
    if (from_loop) {
        DEBUG_MSG_P(PSTR("[MATTER] heap %s: free %u, block %u, min %u\n"), step,
            static_cast<unsigned>(systemFreeHeap()), static_cast<unsigned>(stats.usable),
            static_cast<unsigned>(systemHeapMinimum()));
        return;
    }

    char line[log::Length];
    snprintf(line, sizeof(line), "heap %s: free %u, block %u, min %u", step,
        static_cast<unsigned>(systemFreeHeap()), static_cast<unsigned>(stats.usable),
        static_cast<unsigned>(systemHeapMinimum()));
    log::push(line);
}

} // namespace matter
} // namespace espurna

// ---------------------------------------------------------------------------------------------------------------------
// Callbacks of the Matter application (espurna_matter.h)

extern "C" {

void matter_onoff_changed(uint8_t endpoint, bool on) {
    using namespace espurna::matter;
    if ((endpoint < 1) || (endpoint > MaxPlugs)) {
        return;
    }

    events.push(Event{Event::Type::OnOff, endpoint, on}, EventWait);
}

void matter_commissioning(bool active) {
    using namespace espurna::matter;
    backend::commissioning(active);
    commissioning_active = active;
    events.push(Event{Event::Type::Commissioning, 0, active}, EventWait);
}

void matter_window(bool open) {
    using namespace espurna::matter;
    window_open = open;
    events.push(Event{Event::Type::Window, 0, open}, EventWait);
}

void matter_pong(void) {
    using namespace espurna::matter;
    events.push(Event{Event::Type::Pong, 0, true});
}

// Every message in and out is an "EM" progress line, a dozen per exchange: they flood the 8 slots and make the Matter
// task wait. They only pass while a controller commissions, errors of every module always do
void matter_log(const char* line) {
    using namespace espurna::matter;
    const auto now = espurna::time::ms();
    if (strstr(line, "failed to match destination ID")) {
        ++unknown_case.count;
        unknown_case.last = now | 1;
        if (!unknown_case.reported || ((now - unknown_case.reported) >= UnknownCaseReportMs)) {
            char text[log::Length];
            snprintf(text, sizeof(text), "A controller connects with a fabric this device does not have (%u tries): "
                "remove the device in that controller's app and pair it again", static_cast<unsigned>(unknown_case.count));
            log::push(text);
            unknown_case.reported = now | 1;
            unknown_case.count = 0;
        }
        return;
    }

    // the rest of such a try: its Sigma1 lines and the two failures after the mismatch
    if (unknown_case.last && ((now - unknown_case.last) < UnknownCaseFloodMs)
        && (strstr(line, "Sigma1") || strstr(line, "establishment failed: 10") || strstr(line, "failed, err = 10"))) {
        return;
    }

    if ((line[0] == 'E') && (line[1] == 'M') && (line[2] == ' ') && !commissioning_active && !window_open) {
        return;
    }

    // free heap at the time of the line: commissioning runs out of it, this shows which step takes how much
    char tagged[log::Length];
    snprintf(tagged, sizeof(tagged), "(%u) %s", static_cast<unsigned>(systemFreeHeap()), line);
    log::push(tagged);
#if defined(MATTER_UDP_LOG)
    udplog::send(line);
#endif
}

// the library asks once, in its InitServer() after the plugs and the OTA requestor: a heap checkpoint of the start
const char* matter_node_label(void) {
    espurna::matter::heap_step("ota");
    return espurna::matter::node_label;
}

const char* matter_serial_number(void) {
    return espurna::matter::serial_number;
}

const char* matter_product_label(void) {
    return espurna::matter::product_label;
}

// the library asks once, in its InitServer() right after the server init (AppServer::Init): a heap checkpoint
uint8_t matter_plug_count(void) {
    espurna::matter::heap_step("server init");
    return espurna::matter::plug_count;
}

uint16_t matter_device_type(void) {
    return espurna::matter::device_type;
}

bool matter_plug_name(uint8_t plug, char* label, size_t label_size, char* unique, size_t unique_size) {
    using namespace espurna::matter;
    if ((plug < 1) || (plug > plug_count) || !label_size || !unique_size) {
        return false;
    }

    const auto& name = plug_names[plug - 1];
    copy_text(label, label_size, name.label, std::strlen(name.label));
    copy_text(unique, unique_size, name.unique, std::strlen(name.unique));

    return true;
}

} // extern "C"

void matterSetup() {
    espurna::matter::setup();
}

bool matterStarted() {
    return espurna::matter::started;
}

// The matter_ble module (commissioning over BLE, linked against this module): a board without a Wi-Fi network has no
// StationConnected to start Matter from, so the module starts it at once - its pairing window advertises on BLE, the
// controller gives the network (arch/<chip>/matter_<chip>.cpp hands it to ESPurna's Wi-Fi). `matterAuto 0` keeps it
// for MATTER.START, as on the network. false: not started
// A controller that commissions over BLE gives the Wi-Fi network (Network Commissioning, the Matter task): it becomes
// ESPurna's first network (ssid0 / pass0, as the WebUI saves it) and ESPurna's Wi-Fi connects to it, in loop(). Both
// chips (arch/<chip>/matter_<chip>.cpp). false: not queued
bool matterWifiFromController(const char* ssid, size_t ssid_length, const char* pass, size_t pass_length) {
    if (!ssid || !ssid_length) {
        return false;
    }
    espurna::string ssid_copy(ssid, ssid_length);
    espurna::string pass_copy(pass ? pass : "", pass ? pass_length : 0);
    const auto bytes = ssid_copy.length() + pass_copy.length();
    return espurna::system::runInLoop([ssid_copy, pass_copy]() {
        DEBUG_MSG_P(PSTR("[MATTER] Network from the controller: %s\n"), ssid_copy.c_str());
        setSetting({"ssid", 0}, ssid_copy);
        setSetting({"pass", 0}, pass_copy);
        espurnaReload();
    }, bytes);
}

bool matterStartForBle() {
    if (!getSetting("matterAuto", true)) {
        return espurna::matter::started;
    }
    espurna::matter::start();
    return espurna::matter::started;
}

// For the other modules through the head (modules_api.cpp; the nspanel module: the pairing QR on its display). The
// pairing window is open: the basic one, or one a controller opened
bool matterWindowOpen() {
    return espurna::matter::started && (espurna::matter::window_left() > 0);
}

// Opens the basic pairing window, as MATTER.WINDOW and the button of the MATTER panel do (the nspanel module: the "matter-pair"
// tile of the NSPanel). It opens in the Matter task, matterWindowOpen() follows. false: Matter is not started
bool matterWindowStart() {
    if (!espurna::matter::started) {
        return false;
    }

    espurna::matter::open_window();
    return true;
}

// The QR payload ("MT:...") and the manual pairing code, as on the MATTER panel of the WebUI. They carry the setup
// passcode: for the device owner's screens only, never logged
bool matterPairingCodes(char* qr, size_t qr_size, char* manual, size_t manual_size) {
    return espurna::matter::codes(manual, manual_size, qr, qr_size);
}

#endif
