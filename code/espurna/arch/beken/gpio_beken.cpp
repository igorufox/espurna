/*

GPIO MODULE, BK72xx (LibreTiny) part (common part is in gpio.cpp and arch/rtos/gpio_rtos.cpp)

*/

#include "espurna.h"

namespace espurna {
namespace gpio {
namespace {

// BK72xx GPIO config registers, one word per pin (ref. BDK driver/gpio/gpio.h)
// bit 0 input level, bit 1 output level, bit 2 input enable, bit 3 output disable, bit 6 second function
constexpr uintptr_t RegGpioConfig { 0x00802800 };

uint32_t pin_config(unsigned char pin) {
    return *reinterpret_cast<volatile uint32_t*>(RegGpioConfig + (pin * 4));
}

} // namespace

namespace chip {

// LibreTiny pin numbers are GPIO numbers (P6 is 6). Only the pins routed out by the module (ref. LibreTiny board variant)
bool valid(uint8_t pin) {
    return pinSupported(pinInfo(pin), PIN_GPIO);
}

bool valid_output(uint8_t) {
    return true;
}

// ArduinoCore-API has enums instead of plain ints, values are the same
void pin_mode(uint8_t pin, int8_t mode) {
    ::pinMode(pin, static_cast<PinMode>(mode));
}

void write(uint8_t pin, int8_t value) {
    ::digitalWrite(pin, static_cast<PinStatus>(value));
}

// Nothing but the system word survives the reset (ref. rtcmem_beken.cpp), pins are not kept
void setup() {
}

void print_pin(Print& out, uint8_t pin) {
    const auto config = pin_config(pin);
    const bool function = config & (1 << 6);
    const bool output = !(config & (1 << 3));

    out.printf_P(
        PSTR("%8s @ GPIO%02d (%s) 0x%02X\n"),
        function ? PSTR("FUNCTION")
            : output ? PSTR("OUTPUT")
            : PSTR("INPUT"),
        pin,
        (output ? (config & (1 << 1)) : (config & (1 << 0)))
            ? PSTR("HIGH")
            : PSTR("LOW"),
        config & 0xff);
}

} // namespace chip
} // namespace gpio
} // namespace espurna

