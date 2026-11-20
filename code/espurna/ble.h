/*

BLE: the scanner and the GATT server of the ble module (ble.cpp), ESP32 and BK7231N (the ESP8266 has no Bluetooth:
no module there)

The module is the bus of the BLE modules, like the i2c module is the bus of the I2C drivers: the chip's stack, the
passive scan and the GATT server (arch/<chip>/ble_<chip>.cpp, the thin driver of ble_driver.h). The modules on it
(ble_sensors, ble_proxy, ble_wifi, matter_ble) need it and are linked against it directly; they get every
advertisement in loop() through bleOnAdvert(), serve a GATT service of their own through bleServe().

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ble {

// legacy advertising PDU payload; the longer ones of the extended advertising are cut to it
constexpr size_t AdvertDataMax { 31 };

struct Advert {
    uint8_t mac[6];         // in the order it is printed, AA:BB:CC:DD:EE:FF
    uint8_t addr_type;      // 0 public, 1 random
    int8_t rssi;
    uint8_t length;
    uint8_t data[AdvertDataMax];
};

using AdvertCallback = void (*)(const Advert&);

// The GATT server of the board (the modules that talk to a phone or a controller: ble_wifi, matter_ble): up to
// ServicesMax services of the modules, one client at a time. The stack starts after every module did (the first
// loop()): a module registers its service in its entry. Every event comes in loop()
namespace gatt {

// the properties of a characteristic
constexpr uint8_t Read { 1 };
constexpr uint8_t Write { 2 };      // write request and write command (without response)
constexpr uint8_t Notify { 4 };
constexpr uint8_t Indicate { 8 };

constexpr size_t ServicesMax { 2 };
constexpr size_t CharacteristicsMax { 6 };

// the value a client reads (kept by ble.cpp)
constexpr size_t ValueMax { 64 };

// a write of a client, an indication (CHIPoBLE: MTU 247 - 3)
constexpr size_t DataMax { 244 };

// the MTU the server asks for
constexpr uint16_t Mtu { 247 };

constexpr uint16_t NoConnection { 0xffff };

// 16 or 128 bits, as they go on the air (LSB first)
struct Uuid {
    uint8_t size;           // 2 or 16
    uint8_t bytes[16];
};

struct Characteristic {
    Uuid uuid;
    uint8_t props;
};

struct Service {
    Uuid uuid;
    const Characteristic* characteristics;
    size_t count;
};

// what a module hears of its service; any of them may be nullptr. In loop(), or, with `stack_task`, right in the
// task of the stack (CHIPoBLE: the bytes go on to the task of Matter at once; the callback copies what it keeps and
// returns quickly, no waiting, no settings, no web)
struct Events {
    bool stack_task;
    // a client connected / disconnected (reason: the HCI reason of the disconnection, 0 when connected)
    void (*connection)(uint16_t conn, bool connected, uint8_t reason);
    // a client wrote a characteristic of the service (its index in the service)
    void (*written)(uint16_t conn, size_t index, const uint8_t* data, size_t length);
    // the client configuration of a characteristic changed
    void (*subscribed)(uint16_t conn, size_t index, bool notify, bool indicate);
    // the MTU of the connection, after the exchange
    void (*mtu)(uint16_t conn, uint16_t mtu);
    // the client confirmed an indication (ok), or it timed out / failed
    void (*indicated)(uint16_t conn, size_t index, bool ok);
};

} // namespace gatt

} // namespace ble
} // namespace espurna

// every advertisement, in loop(), in the order the stack received them
void bleOnAdvert(espurna::ble::AdvertCallback);

// bleAdvertise(), bleIndicate() and bleDisconnect() may be called from any task (Matter's)

// a service of the GATT server: its id (0 .. ServicesMax - 1), -1 when there is no room (or the stack runs already)
int bleServe(const espurna::ble::gatt::Service&, const espurna::ble::gatt::Events&);

// the advertisement of a service (the AD structures of a legacy advertising PDU and of the scan response: <= 31 bytes
// each, copied), connectable, every min_ms .. max_ms; data nullptr - its advertising stops. The radio advertises one
// at a time: the service that asked last, the one before it again when that one stops
bool bleAdvertise(int service, const uint8_t* data, size_t length, const uint8_t* response, size_t response_length,
    uint16_t min_ms, uint16_t max_ms);

// the value of a characteristic a client reads; notify: sent to the connected client too (when it subscribed)
void bleValue(int service, size_t index, const uint8_t* data, size_t length, bool notify);

// an indication to the client (<= its MTU - 3 bytes); the confirmation comes as Events::indicated. false - not sent
bool bleIndicate(int service, size_t index, uint16_t conn, const uint8_t* data, size_t length);

// drop the connection of the client
void bleDisconnect(uint16_t conn);

// the stack is up and scans
bool bleRunning();

void bleSetup();

// the ble_wifi module (ble_wifi.cpp): Improv Wi-Fi over the GATT server
void bleWifiSetup();

// the ble_proxy module (ble_proxy.cpp): the advertisements, raw, to MQTT
void bleProxySetup();
