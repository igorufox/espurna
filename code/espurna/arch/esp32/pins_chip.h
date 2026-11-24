/*

PINS, ESP32: the facts of the chip's pins for the common code (pins.h, onewire_bus.h)

*/

#pragma once

#include <cstdint>

namespace espurna {
namespace pins {
namespace chip {

// the first ADC pin: GPIO36 of ADC1
constexpr uint8_t Adc0 { 36 };

// an output can be open drain (pin::OutputOpenDrain): yes
constexpr bool OpenDrain { true };

} // namespace chip
} // namespace pins
} // namespace espurna
