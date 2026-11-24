/*

PINS, HOST (the unit tests): the facts of the chip's pins for the common code (pins.h, onewire_bus.h)

*/

#pragma once

#include <cstdint>

namespace espurna {
namespace pins {
namespace chip {

// the first ADC pin: none, a number
constexpr uint8_t Adc0 { 0 };

// an output can be open drain (pin::OutputOpenDrain): as the ESP chips
constexpr bool OpenDrain { true };

} // namespace chip
} // namespace pins
} // namespace espurna
