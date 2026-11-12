/*

The light_pwm module (docs/modules.md): the PWM output of the lights, one channel per GPIO (ltDimmerGPIO0..4, the
chip's PWM driver: pwm.cpp, arch/<chip>/pwm_chip.h). An output of the light module, it needs it and is linked against it
directly: it registers its channels with lightSetProvider() / lightAdd(), the light module does the rest (the values,
the transitions, MQTT, the LIGHTS panel). Taken when ltProvider is `dimmer` or not set (the generic default).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "gpio.h"
#include "light.h"
#include "pwm.h"

#include <memory>
#include <vector>

namespace {

class PwmOutput final : public LightProvider {
public:
    explicit PwmOutput(PwmRange range) :
        _range(range)
    {}

    // 0 duty is the off state, nothing else to do
    void state(bool) override {
    }

    // the light's value [ValueMin, ValueMax] to the duty of the PWM driver
    void channel(size_t ch, float value) override {
        pwmDuty(ch, static_cast<uint32_t>((value - espurna::light::ValueMin) * (_range.max - _range.min)
            / (espurna::light::ValueMax - espurna::light::ValueMin) + _range.min));
    }

    void update() override {
        pwmUpdate();
    }

private:
    PwmRange _range;
};

} // namespace

ESPURNA_MODULE(light_pwm, "light_pwm", 11) {
    const auto provider = getSetting(F("ltProvider"));
    if (provider.length() && !provider.equals(F("dimmer"))) {
        return;
    }

    // up to the first pin that is not set
    std::vector<uint8_t> pins;
    pins.reserve(espurna::light::ChannelsMax);
    for (size_t index = 0; index < espurna::light::ChannelsMax; ++index) {
        const auto pin = getSetting({F("ltDimmerGPIO"), index}, uint8_t{ GPIO_NONE });
        if (!gpioValid(pin)) {
            break;
        }

        pins.push_back(pin);
    }

    // the PWM driver has to agree on the pins
    if (!pins.size() || !pwmInitPins(pins)) {
        return;
    }

    lightSetProvider(std::make_unique<PwmOutput>(pwmRange()));
    for (size_t index = 0; index < pins.size(); ++index) {
        lightAdd();
    }
}

#endif // MODULE_BUILD
