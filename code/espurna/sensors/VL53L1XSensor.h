// -----------------------------------------------------------------------------
// VL53L1X Sensor over I2C
// Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
// -----------------------------------------------------------------------------

#if SENSOR_SUPPORT && VL53L1X_SUPPORT

#pragma once

// the VL53L1X library of the fork igorufox/vl53l1x (code/libraries.ini) without Arduino (VL53L1X_NO_ARDUINO of the
// module flags): its I2C bus and clock are i2c.h and espurna::time, given with setPlatform()
#include <VL53L1X.h>

#include "I2CSensor.h"
#include "../i2c.h"

class VL53L1XSensor : public I2CSensor<> {

    public:

        VL53L1XSensor() {
            _vl53l1x.setPlatform(&Platform);
        }

        using MeasurementTimingBudget = espurna::duration::critical::Microseconds;
        using InterMeasurementPeriod = espurna::duration::Milliseconds;

        // ---------------------------------------------------------------------

        void setDistanceMode(VL53L1X::DistanceMode mode) {
            _vl53l1x.setDistanceMode(mode);
        }

        void setMeasurementTimingBudget(MeasurementTimingBudget value) {
            _vl53l1x.setMeasurementTimingBudget(value.count());
        }

        void setInterMeasurementPeriod(InterMeasurementPeriod value) {
            if (_measurement_period != value) {
                _measurement_period = value;
                _dirty = true;
            }
        }

        // ---------------------------------------------------------------------
        // Sensor API
        // ---------------------------------------------------------------------

        unsigned char id() const override {
            return SENSOR_VL53L1X_ID;
        }

        unsigned char count() const override {
            return 1;
        }

        void begin() override {
            if (!_dirty) {
                return;
            }

            // I2C auto-discover
            static constexpr uint8_t addresses[] {0x29};
            const auto address = findAndLock(addresses);
            if (address == 0) {
                return;
            }

            _vl53l1x.setAddress(address);
            if (!_vl53l1x.init()) {
                return;
            }

            _vl53l1x.startContinuous(_measurement_period.count());

            _ready = true;
            _dirty = false;
        }

        // Descriptive name of the sensor
        espurna::string description() const override {
            char buffer[21];
            snprintf_P(buffer, sizeof(buffer),
                PSTR("VL53L1X @ I2C (0x%02X)"), lockedAddress());
            return espurna::string(buffer);
        }

        // Type for slot # index
        unsigned char type(unsigned char index) const override {
            if (index == 0) return MAGNITUDE_DISTANCE;
            return MAGNITUDE_NONE;
        }

        // Pre-read hook (usually to populate registers with up-to-date data)
        void pre() override {
            if (!_vl53l1x.dataReady()) {
                return;
            }

            _distance = (double) _vl53l1x.read(false) / 1000.00;
        }

        // Current value for slot # index
        double value(unsigned char index) override {
            if (index == 0) {
                return _distance;
            }
            return 0;
        }

    private:

        static uint8_t bus_write(void*, uint8_t address, const uint8_t* data, size_t size) {
            return i2c_write_buffer(address, data, size);
        }

        static size_t bus_read(void*, uint8_t address, uint8_t* data, size_t size) {
            return i2c_read_buffer(address, data, size);
        }

        static uint32_t clock_ms(void*) {
            return espurna::time::ms();
        }

        static void wait_us(void*, uint32_t us) {
            if (us >= 1000) {
                espurna::time::blockingDelay(espurna::duration::Milliseconds(us / 1000));
                us %= 1000;
            }
            espurna::time::sleep_us(us);
        }

        static constexpr VL53L1X_Platform Platform {
            bus_write, bus_read, clock_ms, wait_us, nullptr,
        };

        InterMeasurementPeriod _measurement_period;
        VL53L1X _vl53l1x;
        double _distance = 0;

};

#endif // SENSOR_SUPPORT && VL53L1X_SUPPORT
