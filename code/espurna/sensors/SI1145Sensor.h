// -----------------------------------------------------------------------------
// SI1145 Sensor over I2C
// Copyright (C) 2020 by @HilverinkJ (https://github.com/HilverinkJ)
// Based on https://github.com/xoseperez/espurna/issues/2192#issuecomment-603430308
// The registers and the start sequence of the Adafruit SI1145 library (Limor Fried, BSD), on i2c.h (no library, no
// Arduino emulation)
// -----------------------------------------------------------------------------

#if SENSOR_SUPPORT && SI1145_SUPPORT

#pragma once

#include "I2CSensor.h"
#include "../i2c.h"

class SI1145Sensor : public I2CSensor<> {

     public:

         unsigned char id() const override {
            return SENSOR_SI1145_ID;
         }

         unsigned char count() const override {
            return 1;
         }

         void begin() override {
             const auto address = findAndLock();
             if (address == 0) {
                 return;
             }

             if (!start(address)) {
                 _ready = false;
                 return;
             }

             _error = SENSOR_ERROR_OK;
             _ready = true;
         }

         // ---------------------------------------------------------------------
         // Sensor API
         // ---------------------------------------------------------------------

         // Descriptive name of the sensor
         espurna::string description() const override {
             char buffer[32];
             snprintf_P(buffer, sizeof(buffer),
                PSTR("SI1145 @ I2C (0x%02X)"), lockedAddress());
             return espurna::string(buffer);
         }

         // Descriptive name of the slot # index
         espurna::string description(unsigned char index) const override {
             return description();
         };

         // Type for slot # index
         unsigned char type(unsigned char index) const override {
             if (index == 0) return MAGNITUDE_UVI;
             return MAGNITUDE_NONE;
         }

         // Pre-read hook (usually to populate registers with up-to-date data)
         void pre() override {
             // the UV index * 100
             _uvi = i2c_read_uint16_le(lockedAddress(), RegUvIndex) / 100.0;
         }

         // Current value for slot # index
         double value(unsigned char index) override {
             if (index == 0) return _uvi;
             return 0.0;
         }

     private:

         static constexpr uint8_t RegPartId { 0x00 };
         static constexpr uint8_t RegIntCfg { 0x03 };
         static constexpr uint8_t RegIrqEn { 0x04 };
         static constexpr uint8_t RegIrqMode1 { 0x05 };
         static constexpr uint8_t RegIrqMode2 { 0x06 };
         static constexpr uint8_t RegHwKey { 0x07 };
         static constexpr uint8_t RegMeasRate0 { 0x08 };
         static constexpr uint8_t RegMeasRate1 { 0x09 };
         static constexpr uint8_t RegPsLed21 { 0x0f };
         static constexpr uint8_t RegUcoeff0 { 0x13 };
         static constexpr uint8_t RegParamWr { 0x17 };
         static constexpr uint8_t RegCommand { 0x18 };
         static constexpr uint8_t RegIrqStat { 0x21 };
         static constexpr uint8_t RegUvIndex { 0x2c };
         static constexpr uint8_t RegParamRd { 0x2e };

         static constexpr uint8_t CommandReset { 0x01 };
         static constexpr uint8_t CommandPsAlsAuto { 0x0f };
         static constexpr uint8_t CommandParamSet { 0xa0 };

         static constexpr uint8_t ParamChList { 0x01 };
         static constexpr uint8_t ParamPsLed12Sel { 0x02 };
         static constexpr uint8_t ParamPs1AdcMux { 0x07 };
         static constexpr uint8_t ParamPsAdcCounter { 0x0a };
         static constexpr uint8_t ParamPsAdcGain { 0x0b };
         static constexpr uint8_t ParamPsAdcMisc { 0x0c };
         static constexpr uint8_t ParamAlsIrAdcMux { 0x0e };
         static constexpr uint8_t ParamAlsVisAdcCounter { 0x10 };
         static constexpr uint8_t ParamAlsVisAdcGain { 0x11 };
         static constexpr uint8_t ParamAlsVisAdcMisc { 0x12 };
         static constexpr uint8_t ParamAlsIrAdcCounter { 0x1d };
         static constexpr uint8_t ParamAlsIrAdcGain { 0x1e };
         static constexpr uint8_t ParamAlsIrAdcMisc { 0x1f };

         static void write(uint8_t address, uint8_t reg, uint8_t value) {
             i2c_write_uint8(address, reg, value);
         }

         static void param(uint8_t address, uint8_t param, uint8_t value) {
             write(address, RegParamWr, value);
             write(address, RegCommand, param | CommandParamSet);
             i2c_read_uint8(address, RegParamRd);
         }

         static bool start(uint8_t address) {
             if (i2c_read_uint8(address, RegPartId) != 0x45) {
                 return false;
             }

             // reset
             write(address, RegMeasRate0, 0);
             write(address, RegMeasRate1, 0);
             write(address, RegIrqEn, 0);
             write(address, RegIrqMode1, 0);
             write(address, RegIrqMode2, 0);
             write(address, RegIntCfg, 0);
             write(address, RegIrqStat, 0xff);
             write(address, RegCommand, CommandReset);
             espurna::time::blockingDelay(espurna::duration::Milliseconds(10));
             write(address, RegHwKey, 0x17);
             espurna::time::blockingDelay(espurna::duration::Milliseconds(10));

             // the UV coefficients
             write(address, RegUcoeff0, 0x29);
             write(address, RegUcoeff0 + 1, 0x89);
             write(address, RegUcoeff0 + 2, 0x02);
             write(address, RegUcoeff0 + 3, 0x00);

             // UV, IR, visible, proximity 1
             param(address, ParamChList, 0x80 | 0x20 | 0x10 | 0x01);
             write(address, RegIntCfg, 0x01);
             write(address, RegIrqEn, 0x01);

             // proximity: 20 mA on LED 1, the large IR photodiode
             write(address, RegPsLed21, 0x03);
             param(address, ParamPs1AdcMux, 0x03);
             param(address, ParamPsLed12Sel, 0x01);
             param(address, ParamPsAdcGain, 0);
             param(address, ParamPsAdcCounter, 0x70);
             param(address, ParamPsAdcMisc, 0x20 | 0x04);

             // IR: the small photodiode, high range
             param(address, ParamAlsIrAdcMux, 0x00);
             param(address, ParamAlsIrAdcGain, 0);
             param(address, ParamAlsIrAdcCounter, 0x70);
             param(address, ParamAlsIrAdcMisc, 0x20);

             // visible: high range
             param(address, ParamAlsVisAdcGain, 0);
             param(address, ParamAlsVisAdcCounter, 0x70);
             param(address, ParamAlsVisAdcMisc, 0x20);

             // 255 * 31.25 us = 8 ms, measure on its own
             write(address, RegMeasRate0, 0xff);
             write(address, RegCommand, CommandPsAlsAuto);

             return true;
         }

         double _uvi = 0.0;

};

#endif // SENSOR_SUPPORT && SI1145_SUPPORT
