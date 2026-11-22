// -----------------------------------------------------------------------------
// VEML6075 Sensor over I2C
// Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
// The registers and the UVA / UVB / UV index formulas of the SparkFun VEML6075 library (Vishay's application note), on
// i2c.h (no library, no Arduino emulation)
// -----------------------------------------------------------------------------

#if SENSOR_SUPPORT && VEML6075_SUPPORT

#pragma once

#include "I2CSensor.h"
#include "../i2c.h"

class VEML6075Sensor : public I2CSensor<> {

    public:

        // 0..4: 50, 100, 200, 400, 800 ms
        void setIntegrationTime(int value) {
            _integration = ((value >= 0) && (value <= 4)) ? value : 1;
        }

        void setDynamicMode(bool high) {
            _high = high;
        }

        // ---------------------------------------------------------------------
        // Sensor API
        // ---------------------------------------------------------------------

        unsigned char id() const override {
            return SENSOR_VEML6075_ID;
        }

        unsigned char count() const override {
            return 3;
        }

        void begin() override {
            _ready = false;

            const auto address = findAndLock(Address);
            if (address == 0) {
                return;
            }

            if ((i2c_read_uint16_le(address, RegId) & 0xff) != DeviceId) {
                return;
            }

            // powered on, the integration time, the dynamic mode, no auto-force (it measures all the time)
            uint16_t conf = i2c_read_uint16_le(address, RegConf);
            conf &= ~(ShutdownMask | ItMask | HdMask | AfMask);
            conf |= static_cast<uint16_t>(_integration << ItShift);
            if (_high) {
                conf |= HdMask;
            }
            write_conf(address, conf);

            _ready = true;
        }

        // Descriptive name of the sensor
        espurna::string description() const override {
            char buffer[25];
            snprintf_P(buffer, sizeof(buffer),
                PSTR("VEML6075 @ I2C (0x%02X)"), lockedAddress());
            return espurna::string(buffer);
        }

        // Type for slot # index
        unsigned char type(unsigned char index) const override {
            if (index == 0) return MAGNITUDE_UVA;
            if (index == 1) return MAGNITUDE_UVB;
            if (index == 2) return MAGNITUDE_UVI;
            return MAGNITUDE_NONE;
        }

        // Pre-read hook (usually to populate registers with up-to-date data)
        void pre() override {
            _error = SENSOR_ERROR_OK;

            const auto address = lockedAddress();
            const double raw_a = i2c_read_uint16_le(address, RegUva);
            const double raw_b = i2c_read_uint16_le(address, RegUvb);
            const double comp1 = i2c_read_uint16_le(address, RegComp1);
            const double comp2 = i2c_read_uint16_le(address, RegComp2);

            _uva = raw_a - (2.22 * comp1) - (1.33 * comp2);
            _uvb = raw_b - (2.95 * comp1) - (1.75 * comp2);

            // the responsivity of the 100 ms time, scaled for the others
            static constexpr double Scale[] { 0.5016286645, 1.0, 2.039087948, 3.781758958, 7.371335505 };
            const double uvia = _uva * (0.001111 / Scale[_integration]);
            const double uvib = _uvb * (0.00125 / Scale[_integration]);
            _index = (uvia + uvib) / 2.0;
            if (_high) {
                _index *= 2.0;
            }
        }

        // Current value for slot # index
        double value(unsigned char index) override {
            if (index == 0) return _uva;
            if (index == 1) return _uvb;
            if (index == 2) return _index;
            return 0;
        }

    private:

        static constexpr uint8_t Address { 0x10 };
        static constexpr uint8_t DeviceId { 0x26 };

        static constexpr uint8_t RegConf { 0x00 };
        static constexpr uint8_t RegUva { 0x07 };
        static constexpr uint8_t RegUvb { 0x09 };
        static constexpr uint8_t RegComp1 { 0x0a };
        static constexpr uint8_t RegComp2 { 0x0b };
        static constexpr uint8_t RegId { 0x0c };

        static constexpr uint16_t ShutdownMask { 0x01 };
        static constexpr uint16_t AfMask { 0x02 };
        static constexpr uint16_t HdMask { 0x08 };
        static constexpr uint16_t ItMask { 0x70 };
        static constexpr int ItShift { 4 };

        // the registers are 16 bit, the low byte first
        static void write_conf(uint8_t address, uint16_t value) {
            const uint8_t data[2] {
                static_cast<uint8_t>(value & 0xff),
                static_cast<uint8_t>(value >> 8),
            };
            i2c_write_buffer(address, RegConf, data, sizeof(data));
        }

        int _integration { 1 };
        bool _high { false };

        double _uva { 0 };
        double _uvb { 0 };
        double _index { 0 };

};

#endif // SENSOR_SUPPORT && VEML6075_SUPPORT
