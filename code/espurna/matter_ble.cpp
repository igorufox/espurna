/*

MATTER OVER BLE: commissioning of a board that is not on Wi-Fi yet (CHIPoBLE), the matter_ble module (owner 2026-11-19,
«оба варианта как модули который может и не быть»: this one and Improv Wi-Fi, the ble_wifi module). A Matter
controller (a phone, a speaker hub) finds the board by its BLE advertisement, commissions it over BLE and gives it the
Wi-Fi network (Network Commissioning: it becomes ESPurna's first network, matterWifiFromController() of matter.cpp).

The Matter library has connectedhomeip's BLE transport and its BLEManagerImpl, no BLE stack (matter-espurna
app/matter_ble.h): this module serves the CHIPoBLE GATT service on the ble module's stack (ble.h gatt) and hands the
library the operations (advertise, indicate, disconnect) and the stack's events. Without the module Matter runs on the
network only, as before. Linked against the ble and the matter modules directly.

    service   16-bit UUID 0xFFF6
    C1        18EE2EF5-263D-4559-959F-4F9C429F9D11  write (the controller writes here)
    C2        18EE2EF5-263D-4559-959F-4F9C429F9D12  indicate (the library sends here)

The advertisement and the scan response are the library's (the Matter service data: discriminator, VID, PID; the name
MATTER-<discriminator>), sent while its pairing window is open and nobody is connected. A board without a Wi-Fi network
has no station connection to start Matter from: the module starts it at once (matterStartForBle()).

*/

#include "espurna.h"

#if MODULE_BUILD && MATTER_SUPPORT

#include "ble.h"
#include "espurna_matter.h"
#include "espurna_tasks.h"
#include "settings.h"

#include <app/matter_ble.h>

#include <iterator>

namespace espurna {
namespace matter {
namespace ble {
namespace {

namespace gatt = espurna::ble::gatt;

// the characteristics of the service, by their index (the UUIDs as they go on the air, LSB first)
constexpr size_t Rx { 0 };   // C1
constexpr size_t Tx { 1 };   // C2

constexpr gatt::Characteristic Characteristics[] {
    {{16, {0x11, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE, 0x18}}, gatt::Write},
    {{16, {0x12, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE, 0x18}}, gatt::Indicate},
};

constexpr gatt::Service Service {
    {2, {0xF6, 0xFF}},
    Characteristics,
    std::size(Characteristics),
};

int service { -1 };

// --- what the library asks of the stack (the Matter task)

int advertise(const uint8_t* adv, size_t adv_length, const uint8_t* response, size_t response_length,
        uint16_t min_ms, uint16_t max_ms)
{
    return bleAdvertise(service, adv, adv_length, response, response_length, min_ms, max_ms) ? 0 : -1;
}

int indicate(uint16_t connection, const uint8_t* data, size_t length) {
    return bleIndicate(service, Tx, connection, data, length) ? 0 : -1;
}

int disconnect(uint16_t connection) {
    bleDisconnect(connection);
    return 0;
}

constexpr matter_ble_ops Ops {
    advertise,
    indicate,
    disconnect,
};

// --- what the stack reports (its own task, Events::stack_task): to the library, which takes it into the Matter task

void on_connection(uint16_t connection, bool connected, uint8_t reason) {
    if (connected) {
        matter_ble_connected(connection);
    } else {
        matter_ble_disconnected(connection, reason);
    }
}

void on_written(uint16_t connection, size_t index, const uint8_t* data, size_t length) {
    if (index == Rx) {
        matter_ble_written(connection, data, length);
    }
}

void on_subscribed(uint16_t connection, size_t index, bool notify, bool indicate) {
    if (index == Tx) {
        matter_ble_subscribed(connection, notify || indicate);
    }
}

void on_mtu(uint16_t connection, uint16_t mtu) {
    matter_ble_mtu(connection, mtu);
}

void on_indicated(uint16_t connection, size_t index, bool ok) {
    if (index == Tx) {
        matter_ble_indicated(connection, ok);
    }
}

// from the stack's task, not loop(): the library copies a write into a packet buffer and posts it to the Matter task
// at once (BTP waits for nobody); nothing here takes long or much stack
constexpr gatt::Events Events {
    true,
    on_connection,
    on_written,
    on_subscribed,
    on_mtu,
    on_indicated,
};

} // namespace

void setup() {
    service = bleServe(Service, Events);
    if (service < 0) {
        DEBUG_MSG_P(PSTR("[MATTER] BLE: no room for the Matter service\n"));
        return;
    }

    matter_ble_attach(&Ops);
    DEBUG_MSG_P(PSTR("[MATTER] BLE commissioning available\n"));

    // no Wi-Fi network yet: nothing would start Matter, start it from loop() (the modules are set up, the BLE stack
    // starts in the first loop() too); its pairing window advertises on BLE
    if (!getSetting({"ssid", 0}).length()) {
        espurna::system::runInLoop([]() {
            matterStartForBle();
        });
    }
}

} // namespace ble
} // namespace matter
} // namespace espurna

void matterBleSetup() {
    espurna::matter::ble::setup();
}

#endif
