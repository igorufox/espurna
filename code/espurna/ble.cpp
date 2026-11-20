/*

BLE: the scanner of the ble module (ble.h), the common part of ESP32 and BK7231N. The chip's stack and the scan are
arch/<chip>/ble_<chip>.cpp (ble_driver.h).

Settings
    bleActive       scan requests (the names of the devices in their scan responses), default 0: passive, the radio
                    only listens
    bleInterval     the scan interval, ms (default 300)
    bleWindow       the part of the interval the radio listens, ms (default 60: the Wi-Fi keeps the rest of the radio)

Terminal
    BLE.DEVICES     the devices heard lately: MAC, RSSI, seconds since the last advertisement, the sensor format,
                    the name

The GATT server (ble.h gatt): one service of a module, its values here (the stack's task reads them under a critical
section), the writes of a client into the queue of loop(). The stack starts in the first loop(): every module has
registered its service by then.

*/

#include "espurna.h"

#if MODULE_BUILD

#include "ble.h"
#include "ble_adv.h"
#include "ble_driver.h"
#include "espurna_tasks.h"
#include "terminal.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace espurna {
namespace ble {
namespace {

namespace build {

constexpr size_t QueueMax { 32 };
constexpr size_t DevicesMax { 32 };
constexpr size_t NameMax { 20 };

constexpr bool active() {
    return false;
}

constexpr uint16_t interval() {
    return 300;
}

constexpr uint16_t window() {
    return 60;
}

} // namespace build

namespace settings {

driver::ScanParams scan() {
    driver::ScanParams out;
    out.active = getSetting("bleActive", build::active());
    out.interval_ms = getSetting("bleInterval", build::interval());
    out.window_ms = getSetting("bleWindow", build::window());

    if (out.interval_ms < 3) {
        out.interval_ms = 3;
    } else if (out.interval_ms > 10240) {
        out.interval_ms = 10240;
    }

    if (!out.window_ms || (out.window_ms > out.interval_ms)) {
        out.window_ms = out.interval_ms;
    }

    return out;
}

} // namespace settings

espurna::system::EventQueue<Advert, build::QueueMax> queue;

std::vector<AdvertCallback> callbacks;

// --- the GATT server

// an event of the stack's task for loop()
struct Event {
    enum class Type : uint8_t {
        Connection,
        Written,
        Subscribed,
        Mtu,
        Indicated,
    };

    Type type;
    uint8_t service;
    uint8_t index;
    uint8_t flags;          // connected, notify | indicate << 1, ok; the disconnection reason in `value`
    uint16_t conn;
    uint16_t value;         // the MTU, the reason
    uint16_t length;
    uint8_t data[gatt::DataMax];
};

struct Value {
    uint8_t length;
    uint8_t data[gatt::ValueMax];
};

struct Advertising {
    bool active;
    uint8_t data[AdvertDataMax];
    size_t length;
    uint8_t response[AdvertDataMax];
    size_t response_length;
    uint16_t min_ms;
    uint16_t max_ms;
};

namespace server {

const gatt::Service* services[gatt::ServicesMax] {};
gatt::Events events[gatt::ServicesMax] {};
size_t count { 0 };

Value values[gatt::ServicesMax][gatt::CharacteristicsMax];

Advertising advertising[gatt::ServicesMax] {};
// the services that advertise, the last one asked last: it is on the air
int order[gatt::ServicesMax] { -1, -1 };

espurna::system::EventQueue<Event, 6> queue;

} // namespace server

bool started { false };

// the advertisement on the air: of the service that asked last and still advertises
void advertise() {
    if (!started) {
        return;
    }

    for (size_t position = gatt::ServicesMax; position > 0; --position) {
        const int service = server::order[position - 1];
        if ((service >= 0) && server::advertising[service].active) {
            const auto& adv = server::advertising[service];
            driver::advertise(adv.data, adv.length, adv.response, adv.response_length, adv.min_ms, adv.max_ms);
            return;
        }
    }

    driver::advertise(nullptr, 0, nullptr, 0, 0, 0);
}

// to the services that hear it in this task: the stack's (Events::stack_task) or loop()
void dispatch(const Event& event, bool stack_task) {
    const auto mine = [&](size_t service) {
        return (service < server::count) && (server::events[service].stack_task == stack_task);
    };

    switch (event.type) {
    case Event::Type::Connection:
    case Event::Type::Mtu:
        // every service hears of the client
        for (size_t service = 0; service < server::count; ++service) {
            if (!mine(service)) {
                continue;
            }
            const auto& events = server::events[service];
            if ((event.type == Event::Type::Connection) && events.connection) {
                events.connection(event.conn, event.flags != 0, static_cast<uint8_t>(event.value));
            } else if ((event.type == Event::Type::Mtu) && events.mtu) {
                events.mtu(event.conn, event.value);
            }
        }
        break;

    case Event::Type::Written:
        if (mine(event.service) && server::events[event.service].written) {
            server::events[event.service].written(event.conn, event.index, event.data, event.length);
        }
        break;

    case Event::Type::Subscribed:
        if (mine(event.service) && server::events[event.service].subscribed) {
            server::events[event.service].subscribed(event.conn, event.index,
                (event.flags & 1) != 0, (event.flags & 2) != 0);
        }
        break;

    case Event::Type::Indicated:
        if (mine(event.service) && server::events[event.service].indicated) {
            server::events[event.service].indicated(event.conn, event.index, event.flags != 0);
        }
        break;
    }
}

void post(Event::Type type, size_t service, size_t index, uint16_t conn, uint8_t flags, uint16_t value,
    const uint8_t* data, size_t length)
{
    Event event;
    event.type = type;
    event.service = static_cast<uint8_t>(service);
    event.index = static_cast<uint8_t>(index);
    event.flags = flags;
    event.conn = conn;
    event.value = value;
    event.length = static_cast<uint16_t>(std::min(length, gatt::DataMax));
    if (event.length) {
        std::memcpy(event.data, data, event.length);
    }

    // the stack's task: the services that want it there at once, the others in loop()
    dispatch(event, true);
    for (size_t service = 0; service < server::count; ++service) {
        if (!server::events[service].stack_task) {
            server::queue.push(event);
            break;
        }
    }
}

// the devices heard lately, for BLE.DEVICES: the oldest one gives its place to a new one
struct Device {
    uint8_t mac[6];
    int8_t rssi;
    adv::Format format;
    espurna::time::CoreClock::time_point last;
    char name[build::NameMax + 1];
};

Device devices[build::DevicesMax];
size_t devices_used { 0 };

void remember(const Advert& advert) {
    Device* device = nullptr;
    for (size_t index = 0; index < devices_used; ++index) {
        if (std::memcmp(devices[index].mac, advert.mac, sizeof(advert.mac)) == 0) {
            device = &devices[index];
            break;
        }
    }

    if (!device) {
        if (devices_used < build::DevicesMax) {
            device = &devices[devices_used++];
        } else {
            device = &devices[0];
            for (size_t index = 1; index < devices_used; ++index) {
                if (devices[index].last < device->last) {
                    device = &devices[index];
                }
            }
        }

        std::memset(device, 0, sizeof(*device));
        std::memcpy(device->mac, advert.mac, sizeof(advert.mac));
    }

    device->rssi = advert.rssi;
    device->last = espurna::time::millis();

    const auto reading = adv::parse(advert.data, advert.length);
    if (reading.format != adv::Format::None) {
        device->format = reading.format;
    }

    const char* name = nullptr;
    const size_t name_length = adv::name(advert.data, advert.length, &name);
    if (name_length) {
        const size_t length = std::min(name_length, build::NameMax);
        std::memcpy(device->name, name, length);
        device->name[length] = '\0';
    }
}

void start() {
    started = true;

    driver::Services services {};
    for (size_t index = 0; index < server::count; ++index) {
        services.service[index] = server::services[index];
    }
    services.count = server::count;

    const auto params = settings::scan();
    if (!driver::begin(params, services)) {
        DEBUG_MSG_P(PSTR("[BLE] The stack did not start\n"));
        return;
    }

    DEBUG_MSG_P(PSTR("[BLE] Scanning, %s, %hu ms of every %hu ms, GATT services %u\n"),
        params.active ? PSTR("active") : PSTR("passive"), params.window_ms, params.interval_ms, server::count);

    advertise();
}

void loop() {
    if (!started) {
        start();
    }

    Event event;
    while (server::queue.pop(event)) {
        dispatch(event, false);
    }

    const auto dropped = queue.dropped();
    if (dropped) {
        DEBUG_MSG_P(PSTR("[BLE] %u advertisements dropped\n"), dropped);
    }

    Advert advert;
    for (size_t count = 0; (count < build::QueueMax) && queue.pop(advert); ++count) {
        remember(advert);
        for (auto callback : callbacks) {
            callback(advert);
        }
    }
}

#if TERMINAL_SUPPORT
namespace terminal {

PROGMEM_STRING(Devices, "BLE.DEVICES");

void devices_command(::terminal::CommandContext&& ctx) {
    if (!driver::running()) {
        terminalError(ctx, STRING_VIEW("BLE is not running"));
        return;
    }

    const auto now = espurna::time::millis();
    for (size_t index = 0; index < devices_used; ++index) {
        const auto& device = devices[index];
        const auto age = std::chrono::duration_cast<espurna::duration::Seconds>(now - device.last);
        ctx.output.printf_P(PSTR("%02X:%02X:%02X:%02X:%02X:%02X rssi %d, %us ago %s %s\n"),
            device.mac[0], device.mac[1], device.mac[2], device.mac[3], device.mac[4], device.mac[5],
            device.rssi, static_cast<unsigned>(age.count()),
            adv::format_name(device.format), device.name);
    }

    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Devices, devices_command},
};

void setup() {
    espurna::terminal::add(Commands);
}

} // namespace terminal
#endif

} // namespace

namespace driver {

void received(const Advert& advert) {
    queue.push(advert);
}

size_t read(size_t service, size_t index, uint8_t* out, size_t size) {
    if ((service >= gatt::ServicesMax) || (index >= gatt::CharacteristicsMax)) {
        return 0;
    }

    espurna::system::Critical critical;
    const auto& value = server::values[service][index];
    const size_t length = std::min<size_t>(value.length, size);
    std::memcpy(out, value.data, length);
    return length;
}

void written(size_t service, size_t index, uint16_t conn, const uint8_t* data, size_t length) {
    post(Event::Type::Written, service, index, conn, 0, 0, data, length);
}

void connection(uint16_t conn, bool connected, uint8_t reason) {
    post(Event::Type::Connection, 0, 0, conn, connected ? 1 : 0, reason, nullptr, 0);
}

void subscribed(size_t service, size_t index, uint16_t conn, bool notify, bool indicate) {
    post(Event::Type::Subscribed, service, index, conn, (notify ? 1 : 0) | (indicate ? 2 : 0), 0, nullptr, 0);
}

void mtu(uint16_t conn, uint16_t value) {
    post(Event::Type::Mtu, 0, 0, conn, 0, value, nullptr, 0);
}

void indicated(size_t service, size_t index, uint16_t conn, bool ok) {
    post(Event::Type::Indicated, service, index, conn, ok ? 1 : 0, 0, nullptr, 0);
}

} // namespace driver

} // namespace ble
} // namespace espurna

void bleOnAdvert(espurna::ble::AdvertCallback callback) {
    espurna::ble::callbacks.push_back(callback);
}

bool bleRunning() {
    return espurna::ble::driver::running();
}

int bleServe(const espurna::ble::gatt::Service& service, const espurna::ble::gatt::Events& events) {
    using namespace espurna::ble;
    if (started || (server::count >= gatt::ServicesMax) || (service.count > gatt::CharacteristicsMax)) {
        return -1;
    }

    const auto id = server::count++;
    server::services[id] = &service;
    server::events[id] = events;
    return static_cast<int>(id);
}

bool bleAdvertise(int service, const uint8_t* data, size_t length, const uint8_t* response, size_t response_length,
    uint16_t min_ms, uint16_t max_ms)
{
    using namespace espurna::ble;

    // another task (Matter's): a copy to loop(), where the advertising state lives
    if (!espurna::system::loopTask()) {
        struct Copy {
            bool active;
            uint8_t data[AdvertDataMax];
            uint8_t length;
            uint8_t response[AdvertDataMax];
            uint8_t response_length;
        };

        Copy copy {};
        copy.active = data != nullptr;
        if (copy.active) {
            copy.length = static_cast<uint8_t>(std::min(length, AdvertDataMax));
            std::memcpy(copy.data, data, copy.length);
            copy.response_length = static_cast<uint8_t>(response ? std::min(response_length, AdvertDataMax) : 0);
            if (copy.response_length) {
                std::memcpy(copy.response, response, copy.response_length);
            }
        }

        return espurna::system::runInLoop([service, copy, min_ms, max_ms]() {
            bleAdvertise(service, copy.active ? copy.data : nullptr, copy.length,
                copy.response_length ? copy.response : nullptr, copy.response_length, min_ms, max_ms);
        }, sizeof(Copy));
    }
    if ((service < 0) || (static_cast<size_t>(service) >= server::count)) {
        return false;
    }

    auto& adv = server::advertising[service];
    adv.active = data != nullptr;
    if (adv.active) {
        adv.length = std::min(length, AdvertDataMax);
        std::memcpy(adv.data, data, adv.length);
        adv.response_length = response ? std::min(response_length, AdvertDataMax) : 0;
        if (adv.response_length) {
            std::memcpy(adv.response, response, adv.response_length);
        }
        adv.min_ms = min_ms;
        adv.max_ms = (max_ms >= min_ms) ? max_ms : min_ms;

        // the last one that asked goes on the air
        int order[gatt::ServicesMax] { -1, -1 };
        size_t position = 0;
        for (const auto other : server::order) {
            if ((other >= 0) && (other != service)) {
                order[position++] = other;
            }
        }
        order[position] = service;
        std::copy(std::begin(order), std::end(order), std::begin(server::order));
    }

    advertise();
    return true;
}

void bleValue(int service, size_t index, const uint8_t* data, size_t length, bool notify) {
    using namespace espurna::ble;
    if ((service < 0) || (static_cast<size_t>(service) >= server::count) || (index >= gatt::CharacteristicsMax)) {
        return;
    }

    {
        espurna::system::Critical critical;
        auto& value = server::values[service][index];
        value.length = static_cast<uint8_t>(std::min(length, gatt::ValueMax));
        std::memcpy(value.data, data, value.length);
    }

    if (notify && started) {
        driver::notify(service, index);
    }
}

bool bleIndicate(int service, size_t index, uint16_t conn, const uint8_t* data, size_t length) {
    using namespace espurna::ble;
    if (!started || (service < 0) || (static_cast<size_t>(service) >= server::count)
        || (index >= gatt::CharacteristicsMax) || (length > gatt::DataMax))
    {
        return false;
    }

    return driver::indicate(service, index, conn, data, length);
}

void bleDisconnect(uint16_t conn) {
    if (espurna::ble::started) {
        espurna::ble::driver::disconnect(conn);
    }
}

void bleSetup() {
    using namespace espurna::ble;

    espurnaRegisterLoop(loop);
#if TERMINAL_SUPPORT
    espurna::ble::terminal::setup();
#endif
}

#endif // MODULE_BUILD
