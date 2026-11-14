/*

GPIO MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

Common part of every chip. Pins of the chip and how they are driven are in arch/<chip>/gpio_<chip>.cpp

*/

#include "espurna.h"

#include "rtcmem.h"
#include "terminal.h"

#if MCP23S08_SUPPORT
#include "mcp23s08.h"
#endif

#if WEB_SUPPORT
#include "ws.h"
#endif

#include <array>
#include <forward_list>
#include <utility>

namespace espurna {
namespace gpio {
namespace {

enum class ModeOption : int {
    Default = INPUT,
    Output = OUTPUT,
    Input = INPUT,
    InputPulldown = INPUT_PULLDOWN,
    InputPullup = INPUT_PULLUP,
};

namespace settings {
namespace options {

using espurna::settings::options::Enumeration;

PROGMEM_STRING(None, "none");
PROGMEM_STRING(Hardware, "hardware");

#if MCP23S08_SUPPORT
PROGMEM_STRING(Mcp23s08, "mcp23s08");
#endif

static constexpr Enumeration<GpioType> GpioTypeOptions[] PROGMEM {
    {GpioType::Hardware, Hardware},
#if MCP23S08_SUPPORT
    {GpioType::Mcp23s08, Mcp23s08},
#endif
    {GpioType::None, None},
};

PROGMEM_STRING(Default, "default");
PROGMEM_STRING(Output, "output");
PROGMEM_STRING(Input, "input");
PROGMEM_STRING(InputPullup, "input-pull-up");
PROGMEM_STRING(InputPulldown, "input-pull-down");

static constexpr std::array<Enumeration<ModeOption>, 5> ModeOptions PROGMEM {
    {{ModeOption::Default, Default},
     {ModeOption::Output, Output},
     {ModeOption::Input, Input},
     {ModeOption::InputPullup, InputPullup},
     {ModeOption::InputPulldown, InputPulldown}}
};

} // namespace options
} // namespace settings

namespace origin {
namespace internal {

std::forward_list<Origin> origins;

} // namespace internal

void add(Origin origin) {
    internal::origins.remove_if(
        [&](const Origin& other) {
            return other == origin;
        });
    internal::origins.emplace_front(origin);
}

} // namespace origin

#if WEB_SUPPORT
namespace web {

void onVisible(JsonObject& root) {
    JsonObject& config = root.createNestedObject(F("gpioConfig"));

    constexpr GpioType known_types[] {
        GpioType::Hardware,
#if MCP23S08_SUPPORT
        GpioType::Mcp23s08,
#endif
    };

    JsonArray& types = config.createNestedArray(F("types"));

    for (auto& type : known_types) {
        auto* base = gpioBase(type);
        if (base) {
            JsonArray& entry = types.createNestedArray();
            entry.add(base->id());
            entry.add(static_cast<int>(type));

            JsonArray& pins = config.createNestedArray(base->id());
            for (size_t pin = 0; pin < base->pins(); ++pin) {
                if (base->valid(pin)) {
                    pins.add(pin);
                }
            }
        }
    }

    JsonObject& info = root.createNestedObject(F("gpioInfo"));

    JsonArray& locks = info.createNestedArray(F("failed-locks"));
    for (auto& origin : origin::internal::origins) {
        if (!origin.result) {
            JsonArray& entry = locks.createNestedArray();
            entry.add(origin.pin);
            entry.add(origin.location.file);
            entry.add(origin.location.func);
            entry.add(origin.location.line);
        }
    }
}

void setup() {
    wsRegister()
        .onVisible(onVisible);
}

} // namespace web
#endif

#if TERMINAL_SUPPORT
namespace terminal {

volatile uint32_t& reg(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t*>(address);
}

PROGMEM_STRING(GpioLocks, "GPIO.LOCKS");

void gpio_list_origins(::terminal::CommandContext&& ctx) {
    for (const auto& origin : origin::internal::origins) {
        ctx.output.printf_P(PSTR("%c %c %s GPIO%hhu\t%d:%s:%s\n"),
            origin.result ? ' ' : '!',
            origin.lock ? '*' : ' ',
            origin.base, origin.pin,
            origin.location.line,
            origin.location.file,
            origin.location.func);
    }
}

PROGMEM_STRING(Gpio, "GPIO");

int gpio_pin(const String& value) {
    int pin = espurna::settings::internal::convert<int>(value);
    if (pin < 0) {
        return pin;
    }

    if (!gpioValid(pin)) {
        pin = -1;
    }

    return pin;
}

void gpio_read_write(::terminal::CommandContext&& ctx) {
    const auto pin_arg = ctx.argv.size() >= 2;
    const int pin = pin_arg
        ? gpio_pin(ctx.argv[1])
        : -1;

    if (pin_arg && pin < 0) {
        terminalError(ctx, STRING_VIEW("Invalid pin number"));
        return;
    }

    int start = 0;
    int end = gpioPins();

    switch (ctx.argv.size()) {
    case 3:
    {
        // even in INPUT mode, we are still techically allowed to set output register
        // (like it is used in software I2C implementation with INPUT_PULLUP)
        const bool status = espurna::settings::internal::convert<bool>(ctx.argv[2]);
        hardwareGpio().pin(pin)->digitalWrite(status ? HIGH : LOW);
        break;
    }

    case 2:
        start = pin;
        end = pin + 1;
        // fallthrough!

    case 1:
        for (auto current = start; current < end; ++current) {
            if (!gpioValid(current)) {
                continue;
            }

            ctx.output.printf_P(PSTR("%c "), gpioLocked(current) ? '*' : ' ');
            chip::print_pin(ctx.output, current);
        }

        break;

    }

    terminalOK(ctx);
}

PROGMEM_STRING(Mode, "GPIO.MODE");

String gpio_mode_options() {
    String out;

    const auto begin = settings::options::ModeOptions.begin();
    const auto end = settings::options::ModeOptions.end();
    for (auto it = begin; it != end; ++it) {
        if (it != begin) {
            out += '|';
        }

        out += (*it).string();
    }

    return out;
}

void gpio_mode(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 3) {
        const auto pin = gpio_pin(ctx.argv[1]);
        if (pin < 0) {
            terminalError(ctx, STRING_VIEW("Invalid pin number"));
            return;
        }

        const auto mode = espurna::settings::internal::convert(
            settings::options::ModeOptions, ctx.argv[2], ModeOption::Input);
        hardwareGpio().pin(pin)->pinMode(static_cast<int8_t>(mode));

        terminalOK(ctx);
        return;
    }

    static const String hint = STRING_VIEW("GPIO.MODE <GPIO> <").toString()
        + gpio_mode_options()
        + '>';
    terminalError(ctx, hint);
}

PROGMEM_STRING(RegRead, "REG.READ");

void reg_read(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 2) {
        const auto address = espurna::settings::internal::convert<uint32_t>(ctx.argv[1]);
        ctx.output.printf_P(PSTR("0x%08X -> 0x%08X\n"),
            address, reg(address));
        terminalOK(ctx);
        return;
    }

    terminalError(ctx, F("REG.READ <ADDRESS>"));
}

PROGMEM_STRING(RegWrite, "REG.WRITE");

void reg_write(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 3) {
        const auto address = espurna::settings::internal::convert<uint32_t>(ctx.argv[1]);
        const auto value = espurna::settings::internal::convert<uint32_t>(ctx.argv[2]);

        ctx.output.printf_P(PSTR("0x%08X -> 0x%08X\n"), address, value);
        reg(address) = value;

        terminalOK(ctx);
        return;
    }

    terminalError(ctx, F("REG.WRITE <ADDRESS> <VALUE>"));
}

static constexpr espurna::terminal::Command Commands[] PROGMEM {
    {GpioLocks, gpio_list_origins},
    {Gpio, gpio_read_write},
    {Mode, gpio_mode},
    {RegRead, reg_read},
    {RegWrite, reg_write},
};

void setup() {
    espurna::terminal::add(Commands);
}

} // namespace terminal
#endif

} // namespace
} // namespace gpio

namespace settings {
namespace internal {
namespace {

using gpio::settings::options::GpioTypeOptions;

} // namespace

// numbers are accepted as well, earlier ESP32 builds stored them
template <>
GpioType convert(const String& value) {
    return convert(GpioTypeOptions, value, GpioType::None);
}

String serialize(GpioType value) {
    return serialize(GpioTypeOptions, value);
}

} // namespace internal
} // namespace settings
} // namespace espurna

// --------------------------------------------------------------------------

// Per https://llvm.org/docs/CodingStandards.html#provide-a-virtual-method-anchor-for-classes-in-headers
// > If a class is defined in a header file and has a vtable (either it has virtual methods or it derives from classes with virtual methods),
// > it must always have at least one out-of-line virtual method in the class. Without this, the compiler will copy the vtable and RTTI into
// > every .o file that #includes the header, bloating .o file sizes and increasing link times.
// - http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1263r0.pdf
// > This technique is unfortunate as it relies on detailed knowledge of how common toolchains work, and it may also require creating
// > a dummy virtual function.
//
// `~BasePin() = default;` is technically inlined, so leaving that in the header.
// Thus, ::description() implementation here becomes that anchor for the BasePin.

String BasePin::description() const {
    char buffer[64];
    snprintf_P(buffer, sizeof(buffer),
        PSTR("%s @ GPIO%02hhu"), id(), pin());
    return buffer;
}

GpioBase* gpioBase(GpioType type) {
    GpioBase* ptr { nullptr };

    switch (type) {
    case GpioType::Hardware:
        ptr = &hardwareGpio();
        break;
    case GpioType::Mcp23s08:
#if MCP23S08_SUPPORT
        ptr = &mcp23s08Gpio();
#endif
        break;
    case GpioType::None:
        break;
    }

    return ptr;
}

BasePinPtr gpioRegister(GpioBase& base, unsigned char gpio,
        espurna::SourceLocation source_location)
{
    BasePinPtr result;

    if (gpioLock(base, gpio, source_location)) {
        result = base.pin(gpio);
    }

    return result;
}

BasePinPtr gpioRegister(unsigned char gpio, espurna::SourceLocation source_location) {
    return gpioRegister(hardwareGpio(), gpio, source_location);
}

void gpioSetup() {
#if WEB_SUPPORT
    espurna::gpio::web::setup();
#endif
#if TERMINAL_SUPPORT
    espurna::gpio::terminal::setup();
#endif
    espurna::gpio::chip::setup();
}

// Pins whose state is kept through the software reset (ref. arch/<chip>/gpio_<chip>.cpp)
void hardwareGpioIgnore(unsigned char gpio) {
    if (gpio < 64) {
        Rtcmem->gpio_ignore |= (uint64_t{1} << gpio);
    }
}

void gpioLockOrigin(espurna::gpio::Origin origin) {
    espurna::gpio::origin::add(origin);
}
