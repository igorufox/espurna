// -----------------------------------------------------------------------------
// MAX6675 Sensor
// The converter read here: 16 bits clocked out of SO (no library, no Arduino emulation); it measures all the time,
// a read takes the last result
// Copyright (C) 2017-2019 by Xose Pérez <andrade dot luciano at gmail dot com>
// -----------------------------------------------------------------------------

#if SENSOR_SUPPORT && MAX6675_SUPPORT

#pragma once

#include "../pins.h"

#include <vector>

#include "BaseSensor.h"

class MAX6675Sensor : public BaseSensor {

    public:

        void setCS(unsigned char pin_cs) {
            if (_pin_cs == pin_cs) return;
            _pin_cs = pin_cs;
            _dirty = true;
        }

        void setSO(unsigned char pin_so) {
            if (_pin_so == pin_so) return;
            _pin_so = pin_so;
            _dirty = true;
        }

        void setSCK(unsigned char pin_sck) {
            if (_pin_sck == pin_sck) return;
            _pin_sck = pin_sck;
            _dirty = true;
        }

        // ---------------------------------------------------------------------
        // Sensor API
        // ---------------------------------------------------------------------

        unsigned char id() const override {
            return SENSOR_MAX6675_ID;
        }

        unsigned char count() const override {
            return 1;
        }

        // ---------------------------------------------------------------------
        // Initialization method, must be idempotent
        void begin() override {

            if (!_dirty) return;

            espurna::pins::mode(_pin_cs, espurna::pins::Output);
            espurna::pins::mode(_pin_so, espurna::pins::Input);
            espurna::pins::mode(_pin_sck, espurna::pins::Output);
            espurna::pins::write(_pin_sck, false);
            espurna::pins::write(_pin_cs, true);

            _ready = true;
            _dirty = false;

        }

        // Loop-like method, call it in your main loop
        void tick() override {
            const auto now = TimeSource::now();
            if (now - _last_reading > ReadInterval) {
                _last_reading = now;
                _value = read();
            }
        }

        // Descriptive name of the sensor
        espurna::string description() const override {
            return "MAX6675";
        }

        espurna::string address(unsigned char) const override {
            char buffer[16] {0};
            snprintf_P(buffer, sizeof(buffer),
                PSTR("%hhu:%hhu:%hhu"), _pin_cs, _pin_so, _pin_sck);
            return espurna::string(buffer);
        }

        // Type for slot # index
        unsigned char type(unsigned char index) const override {
            if (index == 0) return MAGNITUDE_TEMPERATURE;
            return MAGNITUDE_NONE;
        }

        // Pre-read hook (usually to populate registers with up-to-date data)
        void pre() override {
            _error = _open ? SENSOR_ERROR_OUT_OF_RANGE : SENSOR_ERROR_OK;
        }

        // Current value for slot # index
        double value(unsigned char index) override {
            if (index == 0) {
                return _value;
            }

            return 0.0;
        }

    private:
        using TimeSource = espurna::time::CoreClock;
        static constexpr auto ReadInterval = TimeSource::duration { 3000 };
        TimeSource::time_point _last_reading = TimeSource::now();

        unsigned char _pin_cs = MAX6675_CS_PIN;
        unsigned char _pin_so = MAX6675_SO_PIN;
        unsigned char _pin_sck = MAX6675_SCK_PIN;
        bool _open = false;
        double _value = 0;

        // CS low: D15 dummy, D14..D3 the temperature in 0.25 C, D2 high - the thermocouple is open
        double read() {
            espurna::pins::write(_pin_cs, false);
            espurna::time::sleep_us(1);

            uint16_t raw = 0;
            for (int bit = 0; bit < 16; ++bit) {
                espurna::pins::write(_pin_sck, true);
                espurna::time::sleep_us(1);
                raw = static_cast<uint16_t>((raw << 1) | (espurna::pins::read(_pin_so) ? 1 : 0));
                espurna::pins::write(_pin_sck, false);
                espurna::time::sleep_us(1);
            }

            espurna::pins::write(_pin_cs, true);

            _open = (raw & 0x4) != 0;
            return _open ? _value : static_cast<double>(raw >> 3) * 0.25;
        }

};

#endif // SENSOR_SUPPORT && MAX6675_SUPPORT
