/*

PWM MODULE

Copyright (C) 2019-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#include <chip_sdk.h>

#if PWM_SUPPORT

#include "pwm.h"

#include "pwm_driver.h"

namespace espurna {
namespace driver {
namespace pwm {
namespace build {

constexpr uint32_t frequency() {
    return PWM_FREQUENCY;
}

constexpr uint32_t resolution() {
    return PWM_RESOLUTION;
}

constexpr float limit() {
    return PWM_DUTY_LIMIT;
}

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Frequency, "pwmFreq");
PROGMEM_STRING(Resolution, "pwmRes");
PROGMEM_STRING(Limit, "pwmLimit");

} // namespace keys

namespace {

uint32_t frequency() {
    return getSetting(keys::Frequency, build::frequency());
}

uint32_t resolution() {
    return getSetting(keys::Resolution, build::resolution());
}

float limit() {
    return getSetting(keys::Limit, build::limit());
}

} // namespace
} // namespace settings

namespace {

// TODO: notice that both LEDC and new-pwm API techically support installing multiple pins
// on the same channel, allowing simultaneous pin changes.
// TODO: fade in / out could be implemented here?
// TODO: only consumer right now is lights module, via hw pin <-> channel assignment
// move pin initialization here and expose 'channel-id'? (partially done)

struct Channel {
    uint8_t pin;
    uint32_t duty;
};

namespace internal {

// duty of a fully on channel, the range of the driver values
uint32_t full;

// highest duty that is ever written, pwmLimit percent of the full one
uint32_t duty_limit;

std::vector<Channel> channels;

} // namespace internal

PwmRange range() {
    return PwmRange{
        .min = 0,
        .max = internal::full,
    };
}

// Our internal PINs <-> CHANNELs mapping
size_t channels() {
    return internal::channels.size();
}

// Everything related to PINs is handled in pin init function
// Here, only set up the driver and api range
void setup() {
    const auto full = chip::setup(settings::frequency(), settings::resolution());
    internal::full = full;

    const auto limit = settings::limit();
    internal::duty_limit =
        (limit < 100.f)
            ? (static_cast<float>(full) / 100.f) * limit
            : full;

    if (full != internal::duty_limit) {
        DEBUG_MSG_P(PSTR("[PWM] Duty limit %u (%s%%)\n"),
            internal::duty_limit, formatDecimalString(limit, 3).c_str());
    }
}

void update() {
    const auto full = internal::full;
    for (size_t index = 0; index < internal::channels.size(); ++index) {
        const auto& channel = internal::channels[index];
        chip::write(index, channel.pin, channel.duty, full);
    }

    chip::update();
}

bool valid(const uint8_t* begin, const uint8_t* end) {
    const size_t requested = static_cast<size_t>(end - begin);
    if (!requested) {
        return false;
    }

    if (requested > chip::channels_max()) {
        DEBUG_MSG_P(PSTR("[PWM] Channel budget %u, requested %u\n"),
            (unsigned)chip::channels_max(), (unsigned)requested);
        return false;
    }

    for (auto it = begin; it != end; ++it) {
        if (gpioLocked(*it)) {
            return false;
        }

        if (!chip::pin_valid(*it)) {
            DEBUG_MSG_P(PSTR("[PWM] GPIO%u is not valid\n"), (unsigned)*it);
            return false;
        }

        // Reject duplicates, the driver would install the pin twice
        for (auto prev = begin; prev != it; ++prev) {
            if (*prev == *it) {
                DEBUG_MSG_P(PSTR("[PWM] GPIO%u listed twice\n"), (unsigned)*it);
                return false;
            }
        }
    }

    return true;
}

bool init(const uint8_t* begin, const uint8_t* end) {
    if (!internal::channels.empty()) {
        return false;
    }

    if (!valid(begin, end) || !chip::init(begin, end)) {
        return false;
    }

    for (auto it = begin; it != end; ++it) {
        gpioLock(*it);
        internal::channels.push_back(Channel{
            .pin = *it,
            .duty = 0,
        });
    }

    return true;
}

void duty(size_t channel, uint32_t value) {
    internal::channels[channel].duty =
        std::min(internal::duty_limit, value);
}

void duty(size_t channel, float value) {
    duty(channel, static_cast<uint32_t>(
        (std::clamp(value, 0.f, 100.f) / 100.f) * internal::full));
}

} // namespace

#if TERMINAL_SUPPORT
namespace terminal {

PROGMEM_STRING(PwmWrite, "PWM.WRITE");

void pwm_write(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 3) {
        const auto convert_channel = espurna::settings::internal::convert<uint32_t>;
        const auto channel = convert_channel(ctx.argv[1]);
        if (channel >= channels()) {
            terminalError(ctx, "Invalid channel ID");
            return;
        }

        const auto convert_duty = espurna::settings::internal::convert<float>;
        const auto value = std::clamp(convert_duty(ctx.argv[2]), 0.f, 100.f);
        ctx.output.printf("PWM channel %u duty %s\n",
                channel, formatDecimalString(value, 3).c_str());

        duty(channel, value);
        update();

        terminalOK(ctx);
        return;
    }

    terminalError(ctx, "PWM.WRITE <CHANNEL> <DUTY>");
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {PwmWrite, pwm_write},
};

void setup() {
    espurna::terminal::add(Commands);
}

} // namespace terminal
#endif

} // namespace pwm
} // namespace driver
} // namespace espurna

PwmRange pwmRange() {
    return espurna::driver::pwm::range();
}

size_t pwmChannels() {
    return espurna::driver::pwm::channels();
}

void pwmDuty(size_t channel, uint32_t duty) {
    espurna::driver::pwm::duty(channel, duty);
}

void pwmDuty(size_t channel, float duty) {
    espurna::driver::pwm::duty(channel, duty);
}

bool pwmInitPins(const uint8_t* begin, const uint8_t* end) {
    return espurna::driver::pwm::init(begin, end);
}

void pwmUpdate() {
    espurna::driver::pwm::update();
}

// the PWM of the chip for the head and the other modules (chip_sdk.h PwmBus): pins::pwm(), the NSPanel's buzzer,
// analogWrite() of the Arduino emulation, the head's pin_mode() on Beken
static constexpr espurna::core::chip::PwmBus PwmBusTable {
    &espurna::core::chip::pwm_write,
    &espurna::core::chip::pwm_stop,
};

void pwmSetup() {
    espurna::core::chip::pwm_bus = &PwmBusTable;

    espurna::driver::pwm::setup();
#if TERMINAL_SUPPORT
    espurna::driver::pwm::terminal::setup();
#endif
}

#endif

