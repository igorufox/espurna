/*

GPIO MODULE, ESP8266 on ESP8266_RTOS_SDK (common part is in gpio.cpp and arch/rtos/gpio_rtos.cpp)

*/

#include "espurna.h"

#include <driver/gpio.h>

namespace espurna {
namespace gpio {
namespace chip {

// GPIO 6..11 are bonded to the SPI flash; GPIO 9 and 10 only on the DIO / DOUT boards of the 1 MB layout, still not
// for us (same as the NONOS build: flash pins are not valid for anything, buttons and sensors included)
bool valid(uint8_t pin) {
    if (pin >= 6 && pin <= 11) return false;
    return pin <= 16;
}

bool valid_output(uint8_t pin) {
    return valid(pin);
}

void pin_mode(uint8_t pin, int8_t mode) {
    espurna::pins::mode(pin, static_cast<uint8_t>(mode));
}

void write(uint8_t pin, int8_t value) {
    espurna::pins::write(pin, (value != 0));
}

void setup() {
}

// GPIO_ENABLE (0x6000030C) and GPIO_IN (0x60000318) of GPIO0..15, the RTC pad of GPIO16 (RTC_GPIO_ENABLE 0x60000774,
// RTC_GPIO_IN_DATA 0x6000078C)
void print_pin(::espurna::Output& out, uint8_t pin) {
    bool output;
    bool level;
    if (pin == 16) {
        output = (*reinterpret_cast<volatile uint32_t*>(0x60000774) & 1) != 0;
        level = (*reinterpret_cast<volatile uint32_t*>(0x6000078C) & 1) != 0;
    } else {
        const auto bit = uint32_t{1} << pin;
        output = (*reinterpret_cast<volatile uint32_t*>(0x6000030C) & bit) != 0;
        level = (*reinterpret_cast<volatile uint32_t*>(0x60000318) & bit) != 0;
    }

    out.printf_P(PSTR("%6s @ GPIO%02d%s\n"),
        output ? PSTR("OUTPUT") : PSTR("INPUT"),
        pin,
        output
            ? ""
            : level
                ? PSTR(" (true)")
                : PSTR(" (false)"));
}

} // namespace chip
} // namespace gpio
} // namespace espurna
