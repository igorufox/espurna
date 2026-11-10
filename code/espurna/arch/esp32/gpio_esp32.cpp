/*

GPIO MODULE, ESP32 part (common part is in gpio.cpp)

*/

#include "espurna.h"

#if defined(ESP32)

#include "rtcmem.h"

#include <driver/gpio.h>
#include <soc/gpio_reg.h>

namespace espurna {
namespace gpio {
namespace {

class Hardware final : public HardwareGpioBase<40> {
public:
    // Classic ESP32 wiring constraints:
    //   GPIO 6..11: bonded to the on-chip SPI flash; touching them = boot crash.
    //   GPIO 20, 24, 28..31: do not exist.
    //   GPIO 34..39: input-only, no output driver and no pull resistors.
    // Strapping pins (0, 2, 5, 12, 15) remain valid — they only need
    // attention at reset time.
    // Same as esp8266, flash pins are not valid for anything (incl. buttons and sensors).
    bool valid(unsigned char pin) const override {
        if (pin >= Pins) return false;
        if (pin >= 6 && pin <= 11) return false;
        if (pin == 20 || pin == 24) return false;
        if (pin >= 28 && pin <= 31) return false;
        return true;
    }

    bool validForOutput(unsigned char pin) const override {
        return valid(pin) && (pin < 34);
    }

    BasePinPtr pin(unsigned char pin) override;
};

class HardwarePin final : public ArduinoPin {
public:
    using ArduinoPin::ArduinoPin;

    // Relay pads may still be latched from before the software reset (ref. system_esp32.cpp
    // reboot()). While held, pad ignores any configuration change, so the output stays
    // as it was until the first write sets the output register and the hold is released.
    void pinMode(int8_t mode) override {
        ::pinMode(_pin, mode);
        if (mode != OUTPUT) {
            gpio_hold_dis(static_cast<gpio_num_t>(_pin));
        }
    }

    void digitalWrite(int8_t val) override {
        ::digitalWrite(_pin, val);
        gpio_hold_dis(static_cast<gpio_num_t>(_pin));
    }
};

BasePinPtr Hardware::pin(unsigned char pin) {
    return BasePinPtr(new HardwarePin(pin));
}

} // namespace

namespace chip {

void setup() {
    // Pads latched before the software reset (ref. system_esp32.cpp reboot()).
    // Relay module marks its pins again during setup, anything that is no longer used
    // after the first loop() is released right away instead of staying latched.
    static uint32_t previous;
    previous = Rtcmem->gpio_ignore;
    Rtcmem->gpio_ignore = 0;

    if (previous) {
        espurnaRegisterOnce([]() {
            const auto unused = previous & ~Rtcmem->gpio_ignore;
            for (uint8_t pin = 0; pin < 32; ++pin) {
                if (unused & (uint32_t{1} << pin)) {
                    gpio_hold_dis(static_cast<gpio_num_t>(pin));
                }
            }
        });
    }
}

// GPIO matrix output enable and input registers, GPIO0..31 and GPIO32..39
void print_pin(Print& out, uint8_t pin) {
    const auto bit = uint32_t{1} << (pin % 32);
    const bool output = (REG_READ((pin < 32) ? GPIO_ENABLE_REG : GPIO_ENABLE1_REG) & bit) != 0;
    const bool level = (REG_READ((pin < 32) ? GPIO_IN_REG : GPIO_IN1_REG) & bit) != 0;

    out.printf_P(PSTR("%6s @ GPIO%02d%s\n"),
        output ? PSTR("OUTPUT") : PSTR("INPUT"),
        pin,
        output
            ? ""
            : level
                ? PSTR(" (HIGH)")
                : PSTR(" (LOW)"));
}

} // namespace chip
} // namespace gpio
} // namespace espurna

GpioBase& hardwareGpio() {
    static espurna::gpio::Hardware gpio;
    return gpio;
}

#endif
