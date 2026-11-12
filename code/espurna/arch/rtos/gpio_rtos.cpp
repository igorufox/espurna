/*

GPIO MODULE, part of the FreeRTOS chips (ESP32, BK72xx with LibreTiny)

Pins are driven through the Arduino API. Which pins exist and how a mode or a level reaches the pad is the chip part,
arch/<chip>/gpio_<chip>.cpp (ref. espurna::gpio::chip in gpio.h). Common part of every chip is in gpio.cpp

*/

#include "espurna.h"

#if defined(ESP32) || defined(LIBRETINY)

#include "espurna_isr.h"
#include "gpio.h"

namespace espurna {
namespace gpio {
namespace {

// same pin numbers as the interrupt table, every GPIO of the chip
class Hardware final : public HardwareGpioBase<isr::Pins> {
public:
    bool valid(unsigned char pin) const override {
        return (pin < Pins) && chip::valid(pin);
    }

    bool validForOutput(unsigned char pin) const override {
        return valid(pin) && chip::valid_output(pin);
    }

    BasePinPtr pin(unsigned char pin) override;
};

class HardwarePin final : public BasePin {
public:
    explicit HardwarePin(unsigned char pin) :
        _pin(pin)
    {}

    const char* id() const override {
        return "hardware";
    }

    unsigned char pin() const override {
        return _pin;
    }

    void pinMode(int8_t mode) override {
        chip::pin_mode(_pin, mode);
    }

    void digitalWrite(int8_t val) override {
        chip::write(_pin, val);
    }

    int digitalRead() override {
        return ::digitalRead(_pin);
    }

private:
    unsigned char _pin;
};

BasePinPtr Hardware::pin(unsigned char pin) {
    return BasePinPtr(new HardwarePin(pin));
}

} // namespace
} // namespace gpio
} // namespace espurna

GpioBase& hardwareGpio() {
    static espurna::gpio::Hardware gpio;
    return gpio;
}

#endif
