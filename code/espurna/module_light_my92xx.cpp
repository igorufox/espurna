/*

The light_my92xx module (docs/modules.md): the MY9291 / MY9231 LED drivers of the lights (Ai-Thinker, Arilux ...
bulbs: two pins, DI and DCKI; the my92xx library). An output of the light module, it needs it and is linked against it
directly: it registers its channels with lightSetProvider() / lightAdd(), the light module does the rest. Taken when
ltProvider is `my92xx`.

Settings: ltMy92xxModel (9291, 9231), ltMy92xxChips, ltMy92xxDiGPIO, ltMy92xxDckiGPIO, ltMy92xxChannels (the light
channels, up to 5), ltMy92xxCh0..4 (the driver channel of each light channel)

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "gpio.h"
#include "light.h"

#include <my92xx.h>

#include <memory>

namespace {

// 8 bit commands: the light values are 8 bit
constexpr my92xx_cmd_t Command MY92XX_COMMAND_DEFAULT;
constexpr unsigned int ValueMax { (1u << 8) - 1 };

my92xx_model_t model() {
    // the library's plain enum is one byte with -fshort-enums (LibreTiny): compared by hand
    const auto value = getSetting(F("ltMy92xxModel"));
    if (value.equals(F("9231"))) {
        return MY92XX_MODEL_MY9231;
    }

    return MY92XX_MODEL_MY9291;
}

class My92xxOutput final : public LightProvider {
public:
    My92xxOutput(my92xx_model_t model, int chips, int di, int dcki, size_t channels) :
        _driver(model, chips, di, dcki, Command)
    {
        for (size_t index = 0; index < channels; ++index) {
            _map[index] = getSetting({F("ltMy92xxCh"), index}, static_cast<uint8_t>(index));
        }
    }

    void state(bool value) override {
        _driver.setState(value);
    }

    // the light's value [ValueMin, ValueMax] to the driver's
    void channel(size_t ch, float value) override {
        _driver.setChannel(_map[ch], static_cast<unsigned int>((value - espurna::light::ValueMin) * ValueMax
            / (espurna::light::ValueMax - espurna::light::ValueMin)));
    }

    void update() override {
        _driver.update();
    }

private:
    my92xx _driver;
    uint8_t _map[espurna::light::ChannelsMax] {};
};

} // namespace

ESPURNA_MODULE(light_my92xx, "light_my92xx", 11) {
    if (!getSetting(F("ltProvider")).equals(F("my92xx"))) {
        return;
    }

    const auto channels = std::min<size_t>(getSetting(F("ltMy92xxChannels"), 0), espurna::light::ChannelsMax);
    const auto di = getSetting(F("ltMy92xxDiGPIO"), uint8_t{ GPIO_NONE });
    const auto dcki = getSetting(F("ltMy92xxDckiGPIO"), uint8_t{ GPIO_NONE });
    if (!channels || !gpioValid(di) || !gpioValid(dcki) || !gpioLock(di) || !gpioLock(dcki)) {
        return;
    }

    lightSetProvider(std::make_unique<My92xxOutput>(model(), getSetting(F("ltMy92xxChips"), 1), di, dcki, channels));
    for (size_t index = 0; index < channels; ++index) {
        lightAdd();
    }
}

#endif // MODULE_BUILD
