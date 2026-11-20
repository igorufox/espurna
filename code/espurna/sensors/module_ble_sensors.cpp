/*

SENSOR DRIVER: BLE thermometers (sensors/BleSensor.h): Xiaomi LYWSD03MMC with the ATC / pvvx firmware, BTHome v2
devices, the older Xiaomi thermometers (MiBeacon without a key)
A module of its own (sensors/driver.h): needs the sensors module and the ble module (the scanner, ble.h)

Settings: bleSns0 .. bleSns7, the MAC of a thermometer (AA:BB:CC:DD:EE:FF, the BLE.DEVICES command of the terminal
lists the ones the board hears). One sensor per MAC: temperature, humidity, battery voltage.

*/

#if MODULE_BUILD

#include "driver.h"
#include "../ble.h"
#include "BleSensor.h"

#include <vector>

using namespace espurna::sensor::module;

namespace {

constexpr size_t SensorsMax { 8 };

std::vector<espurna::sensor::driver::ble::Sensor*> sensors;

int hex_digit(char c) {
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    }

    if ((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    }

    if ((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }

    return -1;
}

// AA:BB:CC:DD:EE:FF, AA-BB-..., or AABBCCDDEEFF
bool parse_mac(const espurna::string& text, uint8_t (&out)[6]) {
    size_t byte = 0;
    int high = -1;

    for (char c : text) {
        if ((c == ':') || (c == '-') || (c == ' ')) {
            continue;
        }

        const int digit = hex_digit(c);
        if ((digit < 0) || (byte >= sizeof(out))) {
            return false;
        }

        if (high < 0) {
            high = digit;
        } else {
            out[byte++] = static_cast<uint8_t>((high << 4) | digit);
            high = -1;
        }
    }

    return (byte == sizeof(out)) && (high < 0);
}

void on_advert(const espurna::ble::Advert& advert) {
    for (auto* sensor : sensors) {
        if (sensor->update(advert)) {
            break;
        }
    }
}

} // namespace

ESPURNA_SENSOR_DRIVER(ble_sensors, "bleSns#|text|BLE thermometer #, MAC|||8") {
    for (size_t index = 0; index < SensorsMax; ++index) {
        uint8_t mac[6];
        if (!parse_mac(getSetting({"bleSns", index}), mac)) {
            continue;
        }

        auto* sensor = new espurna::sensor::driver::ble::Sensor(mac);
        sensors.push_back(sensor);
        sensorAdd(sensor);
    }

    if (!sensors.empty()) {
        bleOnAdvert(on_advert);
    }
}

#endif // MODULE_BUILD
