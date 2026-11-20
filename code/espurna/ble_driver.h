/*

BLE DRIVER: what the chip gives the ble module (ble.cpp), one file per chip: arch/esp32/ble_esp32.cpp (the
controller in ROM + NimBLE of ESP-IDF), arch/beken/ble_beken.cpp (the BLE 5.x stack of the BDK). Objects of the ble
module (the chip's line of custom_modules in platformio_<chip>.ini): nothing of it in the head.

The stack runs in a task of its own; the functions of the "chip calls" part are called from there and only copy into
the queues of loop() (espurna_tasks.h EventQueue) or read the values ble.cpp keeps (under a critical section). The
functions of the "ble.cpp calls" part run in loop().

*/

#pragma once

#include "ble.h"

namespace espurna {
namespace ble {
namespace driver {

struct ScanParams {
    bool active;            // scan requests (the names in the scan responses), else passive: the receiver only
    uint16_t interval_ms;   // 3 .. 10240
    uint16_t window_ms;     // <= interval_ms
};

// the services of the GATT server the stack starts with (count 0 - no server)
struct Services {
    const gatt::Service* service[gatt::ServicesMax];
    size_t count;
};

// --- ble.cpp calls these, in loop()

// brings the stack up, the scan and the services of the server; false when the chip has no room for it
bool begin(const ScanParams&, const Services&);

// the stack is up and scans
bool running();

// advertise these bytes (connectable, min .. max ms), or stop (data nullptr): the driver copies them
void advertise(const uint8_t* data, size_t length, const uint8_t* response, size_t response_length,
    uint16_t min_ms, uint16_t max_ms);

// the value of a characteristic changed (ble.cpp keeps it): to the connected client, when it subscribed
void notify(size_t service, size_t index);

// an indication; false - not sent (no client, busy)
bool indicate(size_t service, size_t index, uint16_t conn, const uint8_t* data, size_t length);

void disconnect(uint16_t conn);

// --- the chip calls these, from the stack's task

void received(const Advert&);

// the value a client reads: copied into `out`, its length
size_t read(size_t service, size_t index, uint8_t* out, size_t size);

void written(size_t service, size_t index, uint16_t conn, const uint8_t* data, size_t length);
void connection(uint16_t conn, bool connected, uint8_t reason);
void subscribed(size_t service, size_t index, uint16_t conn, bool notify, bool indicate);
void mtu(uint16_t conn, uint16_t value);
void indicated(size_t service, size_t index, uint16_t conn, bool ok);

} // namespace driver
} // namespace ble
} // namespace espurna
