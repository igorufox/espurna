/*

GPIO MODULE, BK72xx (LibreTiny) part (common part is in gpio.cpp)

*/

#include "espurna.h"

#if defined(LIBRETINY)

namespace espurna {
namespace gpio {
namespace {

// LibreTiny pin numbers are GPIO numbers (P6 is 6)
class Hardware final : public HardwareGpioBase<PINS_GPIO_MAX + 1> {
public:
    // Only the pins routed out by the module (ref. LibreTiny board variant)
    bool valid(unsigned char pin) const override {
        if (pin >= Pins) return false;
        return pinSupported(pinInfo(pin), PIN_GPIO);
    }

    BasePinPtr pin(unsigned char pin) override;
};

class HardwarePin final : public ArduinoPin {
public:
    using ArduinoPin::ArduinoPin;

    // ArduinoCore-API has enums instead of plain ints, values are the same
    void pinMode(int8_t mode) override {
        ::pinMode(_pin, static_cast<PinMode>(mode));
    }

    void digitalWrite(int8_t val) override {
        ::digitalWrite(_pin, static_cast<PinStatus>(val));
    }
};

BasePinPtr Hardware::pin(unsigned char pin) {
    return BasePinPtr(new HardwarePin(pin));
}

// BK72xx GPIO config registers, one word per pin (ref. BDK driver/gpio/gpio.h)
// bit 0 input level, bit 1 output level, bit 2 input enable, bit 3 output disable, bit 6 second function
constexpr uintptr_t RegGpioConfig { 0x00802800 };

uint32_t pin_config(unsigned char pin) {
    return *reinterpret_cast<volatile uint32_t*>(RegGpioConfig + (pin * 4));
}

} // namespace

namespace chip {

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

GpioBase& hardwareGpio() {
    static espurna::gpio::Hardware gpio;
    return gpio;
}

#endif // LIBRETINY
