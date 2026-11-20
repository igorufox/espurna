/*

SENSOR: a BLE thermometer, one per MAC (ble_sensors driver, sensors/module_ble_sensors.cpp): temperature, humidity
and the battery voltage of its advertisements (ble_adv.h: ATC1441, pvvx, BTHome v2, MiBeacon without a key). The
ble module hears them; the sensor keeps the last values and reports an error when the device went silent.

*/

#pragma once

#include "BaseSensor.h"
#include "../ble.h"
#include "../ble_adv.h"

#include <cstring>

namespace espurna {
namespace sensor {
namespace driver {
namespace ble {
namespace {

// no advertisement for that long: the values are stale (thermometers advertise every 2.5 .. 60 s)
constexpr auto Timeout = espurna::duration::Minutes(10);

class Sensor : public BaseSensor {
public:
    static constexpr Magnitude Magnitudes[] {
        {MAGNITUDE_TEMPERATURE},
        {MAGNITUDE_HUMIDITY},
        {MAGNITUDE_VOLTAGE},
    };

    explicit Sensor(const uint8_t (&mac)[6]) {
        std::memcpy(_mac, mac, sizeof(_mac));
    }

    unsigned char id() const override {
        return SENSOR_BLE_ID;
    }

    unsigned char count() const override {
        return std::size(Magnitudes);
    }

    void begin() override {
        _ready = true;
        _error = SENSOR_ERROR_OK;
    }

    espurna::string description() const override {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "BLE %02X:%02X:%02X:%02X:%02X:%02X",
            _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
        return espurna::string(buffer);
    }

    espurna::string address(unsigned char) const override {
        char buffer[18];
        std::snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
            _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
        return espurna::string(buffer);
    }

    unsigned char type(unsigned char index) const override {
        if (index < std::size(Magnitudes)) {
            return Magnitudes[index].type;
        }

        return MAGNITUDE_NONE;
    }

    void pre() override {
        if (!_heard) {
            _error = SENSOR_ERROR_NOT_READY;
            return;
        }

        if ((espurna::time::millis() - _last) > Timeout) {
            _error = SENSOR_ERROR_TIMEOUT;
            return;
        }

        _error = SENSOR_ERROR_OK;
    }

    double value(unsigned char index) override {
        switch (index) {
        case 0:
            return _temperature;
        case 1:
            return _humidity;
        case 2:
            return _voltage;
        }

        return 0.0;
    }

    // from the ble module, in loop()
    bool update(const espurna::ble::Advert& advert) {
        if (std::memcmp(advert.mac, _mac, sizeof(_mac)) != 0) {
            return false;
        }

        const auto reading = espurna::ble::adv::parse(advert.data, advert.length);
        if (!reading) {
            return true;
        }

        if (reading.has_temperature) {
            _temperature = reading.temperature;
        }

        if (reading.has_humidity) {
            _humidity = reading.humidity;
        }

        if (reading.has_voltage) {
            _voltage = reading.voltage;
        }

        _heard = true;
        _last = espurna::time::millis();
        return true;
    }

private:
    uint8_t _mac[6];

    bool _heard { false };
    espurna::time::CoreClock::time_point _last;

    double _temperature { 0.0 };
    double _humidity { 0.0 };
    double _voltage { 0.0 };
};

} // namespace
} // namespace ble
} // namespace driver
} // namespace sensor
} // namespace espurna
