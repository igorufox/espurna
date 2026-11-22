/*

The tables of the I2C and SPI masters and of the PWM (chip_sdk.h I2cBus / SpiBus / PwmBus): the head keeps only where
they are, the i2c / spi / light_pwm modules set them when they start; Wire, SPI and analogWrite() of the Arduino
emulation (arduino_emu), pins::pwm() and the head's pin_mode() (Beken) call through them. Every chip

*/

#include "chip_sdk.h"

namespace espurna {
namespace core {
namespace chip {

const I2cBus* i2c_bus { nullptr };
const SpiBus* spi_bus { nullptr };
const PwmBus* pwm_bus { nullptr };

} // namespace chip
} // namespace core
} // namespace espurna
