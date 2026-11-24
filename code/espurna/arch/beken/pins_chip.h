/*

PINS, Beken: the facts of the chip's pins for the common code (pins.h, onewire_bus.h)

*/

#pragma once

#include <cstdint>

namespace espurna {
namespace pins {
namespace chip {

// the first ADC pin: the first ADC channel
constexpr uint8_t Adc0 { 0 };

// an output can be open drain (pin::OutputOpenDrain): no, the BDK's GPIO has none: a bus line is driven low and let float
constexpr bool OpenDrain { false };

} // namespace chip
} // namespace pins
} // namespace espurna
