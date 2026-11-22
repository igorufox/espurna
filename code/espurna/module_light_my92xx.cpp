/*

The light_my92xx module (docs/modules.md): the MY9291 / MY9231 LED drivers of the lights (Ai-Thinker, Arilux ...
bulbs: two pins, DI and DCKI). An output of the light module, it needs it and is linked against it directly: it
registers its channels with lightSetProvider() / lightAdd(), the light module does the rest. Taken when ltProvider is
`my92xx`.

The drivers are clocked here on espurna::pins (no library, no Arduino emulation): the same sequences as the MY92XX
driver of MaiKe Labs (the chips reset with 32 DCKI pulses each, the command, then the channel values 8 bit, two bits a
DCKI period, latched with DI pulses).

Settings: ltMy92xxModel (9291, 9231), ltMy92xxChips, ltMy92xxDiGPIO, ltMy92xxDckiGPIO, ltMy92xxChannels (the light
channels, up to 5), ltMy92xxCh0..4 (the driver channel of each light channel)

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "gpio.h"
#include "light.h"
#include "pins.h"

#include <memory>
#include <vector>

namespace {

// the command byte: APDM scatter, frequency / 1, 8 bit values, fast reaction, no one-shot (bits 0, 1-2, 3-4, 5, 6)
constexpr uint8_t Command { 0x18 };
constexpr unsigned int ValueMax { (1u << 8) - 1 };

class My92xx {
public:
    // MY9291: 4 channels a chip, MY9231: 3
    My92xx(bool my9231, uint8_t chips, uint8_t di, uint8_t dcki) :
        _chips(chips),
        _di(di),
        _dcki(dcki),
        _values(static_cast<size_t>(chips) * (my9231 ? 3 : 4), 0)
    {
        espurna::pins::mode(_di, espurna::pins::Output);
        espurna::pins::mode(_dcki, espurna::pins::Output);
        espurna::pins::write(_di, false);
        espurna::pins::write(_dcki, false);

        pulse(_dcki, 32 * static_cast<unsigned>(_chips));
        command();
    }

    size_t channels() const {
        return _values.size();
    }

    void value(size_t channel, uint8_t value) {
        if (channel < _values.size()) {
            _values[channel] = value;
        }
    }

    void state(bool value) {
        _state = value;
    }

    void update() {
        espurna::time::sleep_us(12);
        for (const auto value : _values) {
            write(_state ? value : 0);
        }
        espurna::time::sleep_us(12);
        pulse(_di, 8);
        espurna::time::sleep_us(12);
    }

private:
    static void pulse(uint8_t pin, unsigned times) {
        for (unsigned index = 0; index < times; ++index) {
            espurna::pins::write(pin, true);
            espurna::pins::write(pin, false);
        }
    }

    // 8 bits, MSB first, two bits a DCKI period (one on its rising, one on its falling edge)
    void write(uint8_t data) {
        for (int bit = 7; bit > 0; bit -= 2) {
            espurna::pins::write(_dcki, false);
            espurna::pins::write(_di, (data >> bit) & 1);
            espurna::pins::write(_dcki, true);
            espurna::pins::write(_di, (data >> (bit - 1)) & 1);
            espurna::pins::write(_dcki, false);
            espurna::pins::write(_di, false);
        }
    }

    void command() {
        espurna::time::sleep_us(12);
        pulse(_di, 12);
        espurna::time::sleep_us(12);
        for (uint8_t chip = 0; chip < _chips; ++chip) {
            write(Command);
        }
        espurna::time::sleep_us(12);
        pulse(_di, 16);
        espurna::time::sleep_us(12);
    }

    uint8_t _chips;
    uint8_t _di;
    uint8_t _dcki;
    bool _state { false };
    std::vector<uint8_t> _values;
};

class My92xxOutput final : public LightProvider {
public:
    My92xxOutput(bool my9231, uint8_t chips, uint8_t di, uint8_t dcki, size_t channels) :
        _driver(my9231, chips, di, dcki)
    {
        for (size_t index = 0; index < channels; ++index) {
            _map[index] = getSetting({"ltMy92xxCh", index}, static_cast<uint8_t>(index));
        }
    }

    void state(bool value) override {
        _driver.state(value);
    }

    // the light's value [ValueMin, ValueMax] to the driver's
    void channel(size_t ch, float value) override {
        _driver.value(_map[ch], static_cast<uint8_t>((value - espurna::light::ValueMin) * ValueMax
            / (espurna::light::ValueMax - espurna::light::ValueMin)));
    }

    void update() override {
        _driver.update();
    }

private:
    My92xx _driver;
    uint8_t _map[espurna::light::ChannelsMax] {};
};

} // namespace

ESPURNA_MODULE(light_my92xx, "light_my92xx", 11) {
    if (!getSetting("ltProvider").equals("my92xx")) {
        return;
    }

    const auto channels = std::min<size_t>(getSetting("ltMy92xxChannels", 0), espurna::light::ChannelsMax);
    const auto di = getSetting("ltMy92xxDiGPIO", uint8_t{ GPIO_NONE });
    const auto dcki = getSetting("ltMy92xxDckiGPIO", uint8_t{ GPIO_NONE });
    if (!channels || !gpioValid(di) || !gpioValid(dcki) || !gpioLock(di) || !gpioLock(dcki)) {
        return;
    }

    const bool my9231 = getSetting("ltMy92xxModel").equals("9231");
    lightSetProvider(std::make_unique<My92xxOutput>(my9231, getSetting("ltMy92xxChips", uint8_t{ 1 }), di, dcki, channels));
    for (size_t index = 0; index < channels; ++index) {
        lightAdd();
    }
}

#endif // MODULE_BUILD
