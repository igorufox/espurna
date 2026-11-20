/*

GPIO MODULE, part of the FreeRTOS chips (ESP32, BK72xx with LibreTiny)

Pins are driven through the Arduino API. Which pins exist and how a mode or a level reaches the pad is the chip part,
arch/<chip>/gpio_<chip>.cpp (ref. espurna::gpio::chip in gpio.h). Common part of every chip is in gpio.cpp

*/

#include "espurna.h"

#include "espurna_isr.h"
#include "gpio.h"

namespace espurna {
namespace gpio {
namespace {

// every GPIO of the chip: the pins of the interrupt table, and on the ESP8266 GPIO16 too (the RTC pad: a plain
// input / output without an interrupt, gpio_esp8266.cpp; the relay 1 of an LC-Tech x4 board, 11-18)
#if defined(ESP8266)
constexpr size_t HardwarePins { isr::Pins + 1 };
#else
constexpr size_t HardwarePins { isr::Pins };
#endif

class Hardware final : public HardwareGpioBase<HardwarePins> {
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
        return espurna::pins::read(_pin);
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

