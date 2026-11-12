/*

UART MODULE

Copyright (C) 2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

// the head with modules has only the forwarders of uart.h (UART_MODULE), the ports are the uart module
#if UART_SUPPORT && !UART_MODULE

#include "uart.h"
#include "uart_driver.h"
#include "utils.h"

#if WEB_SUPPORT
#include "ws.h"
#endif

#if UART_SOFTWARE_SUPPORT
#include <SoftwareSerial.h>
#endif

#include <array>
#include <utility>

namespace espurna {

namespace settings {
namespace internal {
namespace {

PROGMEM_STRING(ParityNone, "none");
PROGMEM_STRING(ParityEven, "even");
PROGMEM_STRING(ParityOdd, "odd");

static constexpr std::array<settings::options::Enumeration<driver::uart::Parity>, 3> ParityOptions PROGMEM {
    {{driver::uart::Parity::None, ParityNone},
     {driver::uart::Parity::Even, ParityEven},
     {driver::uart::Parity::Odd, ParityOdd}}
};

} // namespace

template <>
driver::uart::Parity convert(const String& value) {
    return convert(ParityOptions, value, driver::uart::Parity::None);
}

String serialize(driver::uart::Parity value) {
    return espurna::settings::internal::serialize(ParityOptions, value);
}

} // namespace internal
} // namespace settings

namespace driver {
namespace uart {
namespace {

namespace types {

#if UART_SOFTWARE_SUPPORT
using SoftwareConfig = ::EspSoftwareSerial::Config;
#endif

} // namespace types

namespace build {

// i.e. uart0, uart1 and a single sw port
// for general use, hardware uart *should* be prefered method
constexpr size_t PortsMax { 3 };

constexpr uint8_t tx(size_t index) {
    return (0 == index) ? (UART1_TX_PIN) :
        (1 == index) ? (UART2_TX_PIN) :
        (2 == index) ? (UART3_TX_PIN) :
            GPIO_NONE;
}

constexpr uint8_t rx(size_t index) {
    return (0 == index) ? (UART1_RX_PIN) :
        (1 == index) ? (UART2_RX_PIN) :
        (2 == index) ? (UART3_RX_PIN) :
            GPIO_NONE;
}

constexpr uint32_t baudrate(size_t index) {
    return (0 == index) ? (UART1_BAUDRATE) :
        (1 == index) ? (UART2_BAUDRATE) :
        (2 == index) ? (UART3_BAUDRATE) :
            0;
}

constexpr uint8_t data_bits(size_t index) {
    return (0 == index) ? (UART1_DATA_BITS) :
        (1 == index) ? (UART2_DATA_BITS) :
        (2 == index) ? (UART3_DATA_BITS)
            : 0;
}

constexpr Parity parity(size_t index) {
    return (0 == index) ? (Parity::UART1_PARITY) :
        (1 == index) ? (Parity::UART2_PARITY) :
        (2 == index) ? (Parity::UART3_PARITY)
            : Parity::None;
}

constexpr uint8_t stop_bits(size_t index) {
    return (0 == index) ? (UART1_STOP_BITS) :
        (1 == index) ? (UART2_STOP_BITS) :
        (2 == index) ? (UART3_STOP_BITS)
            : 0;
}

constexpr bool invert(size_t index) {
    return (0 == index) ? (UART1_INVERT == 1) :
        (1 == index) ? (UART2_INVERT == 1) :
        (2 == index) ? (UART3_INVERT == 1)
            : false;
}

} // namespace build

template <typename T>
constexpr T from_config(Config);

namespace settings {
namespace keys {

PROGMEM_STRING(TxPin, "uartTx");
PROGMEM_STRING(RxPin, "uartRx");

PROGMEM_STRING(Baudrate, "uartBaud");

PROGMEM_STRING(DataBits, "uartDataBits");
PROGMEM_STRING(StopBits, "uartStopBits");
PROGMEM_STRING(Parity, "uartParity");

PROGMEM_STRING(Invert, "uartInv");

// bytes of the receive buffer of a hardware port, 0 - the core's default
PROGMEM_STRING(RxBuffer, "uartRxBuf");

} // namespace keys

} // namespace settings

// The first port leaves its default pin to a button, a relay or an LED set up on it, unless uartTx0 / uartRx0 says
// otherwise: a wall switch on GPIO3 (RX) of a Sonoff Mini R2, relay 1 and the LED on P10 / P11 of a Tuya CBU.
// Without this the port took the pin at boot, before the buttons, and the input never worked
namespace release {

STRING_VIEW_INLINE(Button, "btnGpio");
STRING_VIEW_INLINE(Relay, "relayGpio");
STRING_VIEW_INLINE(RelayButton, "relayBtnGpio");
STRING_VIEW_INLINE(Led, "ledGpio");

// key of the setting that took the pin, empty when the pin stays with the port
String tx_user;
String rx_user;

bool digits(StringView value) {
    if (!value.length()) {
        return false;
    }

    for (const auto c : value) {
        if ((c < '0') || (c > '9')) {
            return false;
        }
    }

    return true;
}

// only <prefix><index> = <pin>, not btnGpioType0 & co
String user(uint8_t pin) {
    String out;
    if (pin == GPIO_NONE) {
        return out;
    }

    espurna::settings::foreach_prefix(
        [&](StringView prefix, String key, const espurna::settings::kvs_type::ReadResult& value) {
            if (out.length()) {
                return;
            }

            if (!digits(StringView(key.begin() + prefix.length(), key.end()))) {
                return;
            }

            const auto pin_value = value.toString();
            if (digits(pin_value) && (static_cast<uint8_t>(pin_value.toInt()) == pin)) {
                out = std::move(key);
            }
        },
        {Button, Relay, RelayButton, Led});

    return out;
}

void setup() {
    if (!hasSetting(espurna::settings::Key{settings::keys::TxPin, size_t{0}})) {
        tx_user = user(build::tx(0));
    }

    if (!hasSetting(espurna::settings::Key{settings::keys::RxPin, size_t{0}})) {
        rx_user = user(build::rx(0));
    }

    if (tx_user.length()) {
        DEBUG_MSG_P(PSTR("[UART] GPIO%hhu is used by %s, the serial port leaves it\n"),
            build::tx(0), tx_user.c_str());
    }

    if (rx_user.length()) {
        DEBUG_MSG_P(PSTR("[UART] GPIO%hhu is used by %s, the serial port leaves it\n"),
            build::rx(0), rx_user.c_str());
    }
}

} // namespace release

namespace settings {

uint8_t tx(size_t index) {
    if ((index == 0) && release::tx_user.length()) {
        return GPIO_NONE;
    }

    return getSetting({keys::TxPin, index}, build::tx(index));
}

uint8_t rx(size_t index) {
    if ((index == 0) && release::rx_user.length()) {
        return GPIO_NONE;
    }

    return getSetting({keys::RxPin, index}, build::rx(index));
}

uint32_t baudrate(size_t index) {
    return getSetting({keys::Baudrate, index}, build::baudrate(index));
}

uint8_t data_bits(size_t index) {
    return getSetting({keys::DataBits, index}, build::data_bits(index));
}

uint8_t stop_bits(size_t index) {
    return getSetting({keys::StopBits, index}, build::stop_bits(index));
}

Parity parity(size_t index) {
    return getSetting({keys::Parity, index}, build::parity(index));
}

bool invert(size_t index) {
    return getSetting({keys::Invert, index}, build::invert(index));
}

size_t rx_buffer(size_t index) {
    return getSetting({keys::RxBuffer, index}, size_t{ 0 });
}

} // namespace settings

using StreamPtr = std::unique_ptr<Stream>;

struct BasePort {
    Type type;
    bool tx;
    bool rx;
    StreamPtr stream;
};

using BasePortPtr = std::unique_ptr<BasePort>;

namespace internal {

BasePortPtr ports[build::PortsMax];

// exclusive owner of the port, ref. claim()
const __FlashStringHelper* owners[build::PortsMax] {};

// serial log and serial terminal
UartClaimRelease on_claim[4] {};

} // namespace internal

// chip UART of these pins, ref. uart_driver.h
BasePortPtr hardware_port(
        uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert, size_t rx_buffer)
{
    const auto opened = hardware::open(baudrate, tx, rx, config, invert, rx_buffer);
    if (!opened.stream) {
        return nullptr;
    }

    return std::make_unique<BasePort>(
        BasePort{
            .type = opened.type,
            .tx = (tx != GPIO_NONE),
            .rx = (rx != GPIO_NONE),
            .stream = StreamPtr(opened.stream),
        });
}

// based on the values in v6 of the lib. still, return bits instead of the octal notation used there
#if UART_SOFTWARE_SUPPORT
constexpr int software_serial_data_bits_from_config(uint8_t bits) {
    return (bits == 5) ? 0 :
        (bits == 6) ? 0b1 :
        (bits == 7) ? 0b10 :
        (bits == 8) ? 0b11 :
            software_serial_data_bits_from_config(8);
}

// btw, SoftwareSerial also has Mark and Space
// no support on the hardware peripheral though (afaik)
constexpr int software_serial_parity_from_config(Parity parity) {
    return (parity == Parity::None) ? 0 :
        (parity == Parity::Even) ? 0b10000 :
        (parity == Parity::Odd) ? 0b11000 :
            software_serial_parity_from_config(Parity::None);
}

constexpr int software_serial_stop_bits_from_config(uint8_t bits) {
    return (bits == 1) ? 0b0 :
        (bits == 2) ? 0b10000000 :
            software_serial_stop_bits_from_config(1);
}

template <>
constexpr types::SoftwareConfig from_config(Config config) {
    return static_cast<types::SoftwareConfig>(
        software_serial_data_bits_from_config(config.data_bits)
        | software_serial_parity_from_config(config.parity)
        | software_serial_stop_bits_from_config(config.stop_bits));

}

BasePortPtr software_serial_port(
        uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert)
{
    const int8_t tx_pin = (tx == GPIO_NONE) ? -1 : tx;
    const int8_t rx_pin = (rx == GPIO_NONE) ? -1 : rx;

    auto* ptr = new SoftwareSerial(rx_pin, tx_pin, invert);
    ptr->begin(baudrate, from_config<types::SoftwareConfig>(config));

    return std::make_unique<BasePort>(
        BasePort{
            .type = Type::Software,
            .tx = (tx_pin > 0),
            .rx = (rx_pin > 0),
            .stream = StreamPtr(ptr),
        });
}
#endif

BasePortPtr make_port(size_t index) {
    BasePortPtr out;

    const auto tx = settings::tx(index);
    const auto rx = settings::rx(index);
    if ((tx == GPIO_NONE) && (rx == GPIO_NONE)) {
        return out;
    }

    if ((tx != GPIO_NONE) && !gpioLock(tx)) {
        return out;
    }

    if ((rx != GPIO_NONE) && !gpioLock(rx)) {
        gpioUnlock(tx);
        return out;
    }

    const auto config = Config{
        .data_bits = settings::data_bits(index),
        .parity = settings::parity(index),
        .stop_bits = settings::stop_bits(index),
    };

    const auto baudrate = settings::baudrate(index);
    const auto invert = settings::invert(index);

    out = hardware_port(baudrate, tx, rx, config, invert, settings::rx_buffer(index));

#if UART_SOFTWARE_SUPPORT
    if (!out) {
        out = software_serial_port(baudrate, tx, rx, config, invert);
    }
#endif

    return out;
}

size_t ports() {
    size_t out = 0;
    for (const auto& port : internal::ports) {
        if (!port) {
            break;
        }
        ++out;
    }

    return out;
}

namespace settings {

STRING_VIEW_INLINE(Prefix, "uart");

namespace query {

#define ID_VALUE(NAME)\
String NAME (size_t id) {\
    return espurna::settings::internal::serialize(\
            espurna::driver::uart::settings::NAME(id));\
}

ID_VALUE(tx)
ID_VALUE(rx)
ID_VALUE(baudrate)
ID_VALUE(data_bits)
ID_VALUE(stop_bits)
ID_VALUE(parity)
ID_VALUE(invert)

#undef ID_VALUE

static constexpr espurna::settings::query::IndexedSetting IndexedSettings[] PROGMEM {
     {keys::TxPin, query::tx},
     {keys::RxPin, query::rx},
     {keys::Baudrate, query::baudrate},
     {keys::DataBits, query::data_bits},
     {keys::StopBits, query::stop_bits},
     {keys::Parity, query::parity},
     {keys::Invert, query::invert},
};

bool checkSamePrefix(StringView key) {
    return key.startsWith(Prefix);
}

espurna::settings::query::Result findFrom(StringView key) {
    return espurna::settings::query::findFrom(
        ports(), IndexedSettings, key);

}

void setup() {
    settingsRegisterQueryHandler({
        .check = checkSamePrefix,
        .get = findFrom,
    });
}

} // namespace query
} // namespace settings

#if TERMINAL_SUPPORT
namespace terminal {
namespace commands {

String port_type(Type type) {
    const char* out = PSTR("UNKNOWN");

    switch (type) {
    case Type::Unknown:
        break;
    case Type::Software:
        out = PSTR("SOFTWARE");
        break;
    case Type::Uart0:
        out = PSTR("UART0");
        break;
    case Type::Uart1:
        out = PSTR("UART1");
        break;
    }

    return out;
}

void uart(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 1) {
        for (size_t index = 0; index < std::size(internal::ports); ++index) {
            const auto& port = internal::ports[index];
            if (!port) {
                break;
            }

            String owner;
            if (internal::owners[index]) {
                owner = F(" owner=");
                owner += internal::owners[index];
            }

            ctx.output.printf_P(
                PSTR("%zu - %s{tx=%c rx=%c}%s\n"),
                index, port_type(port->type).c_str(),
                port->tx ? 'y' : 'n',
                port->rx ? 'y' : 'n',
                owner.c_str());
        }

    } else if (ctx.argv.size() == 2) {
        const auto result = parseUnsigned(ctx.argv[1], 10);
        if (!result.ok) {
            terminalError(ctx, F("Invalid ID"));
            return;
        }

        if (result.value >= ports()) {
            ctx.output.print(F("(Not active)"));
        }

        settingsDump(ctx, settings::query::IndexedSettings, result.value);
    } else {
        terminalError(ctx, F("UART [<ID>]"));
        return;
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Uart, "UART");

static constexpr ::terminal::Command List[] PROGMEM {
    {Uart, commands::uart},
};

} // namespace commands

void setup() {
    espurna::terminal::add(commands::List);
}

} // namespace terminal
#endif

PortPtr port(size_t index) {
    const auto& ptr = internal::ports[index];
    if ((index < std::size(internal::ports)) && (ptr)) {
        return std::make_unique<Port>(
            Port{
                .type = ptr->type,
                .tx = ptr->tx,
                .rx = ptr->rx,
                .stream = ptr->stream.get(),
            });
    }

    return nullptr;
}

// a port of the settings opened now, when it is not yet (a module set it up after the boot opened the others)
PortPtr open(size_t index) {
    if (index >= std::size(internal::ports)) {
        return nullptr;
    }

    if (!internal::ports[index]) {
        internal::ports[index] = make_port(index);
    }

    return port(index);
}

// another speed for an open hardware port (a display that switches its baud rate)
bool baud(size_t index, uint32_t value) {
    if ((index >= std::size(internal::ports)) || !internal::ports[index]) {
        return false;
    }

    const auto& ptr = internal::ports[index];
    return hardware::baud(ptr->stream.get(), ptr->type, value);
}

const __FlashStringHelper* claimed(size_t index) {
    return (index < std::size(internal::owners))
        ? internal::owners[index]
        : nullptr;
}

void on_claim(UartClaimRelease callback) {
    for (auto& ptr : internal::on_claim) {
        if (ptr == callback) {
            return;
        }

        if (!ptr) {
            ptr = callback;
            return;
        }
    }
}

bool claim(size_t index, const __FlashStringHelper* owner) {
    if ((index >= std::size(internal::ports)) || !internal::ports[index] || !owner) {
        return false;
    }

    auto& current = internal::owners[index];
    if (current) {
        if (current == owner) {
            return true;
        }

        DEBUG_MSG_P(PSTR("[UART] Port #%zu is already used by %s\n"),
            index, String(current).c_str());
        return false;
    }

    current = owner;

    // owner is set first, nothing reaches the serial log from here on
    String released;
    for (const auto& callback : internal::on_claim) {
        if (!callback) {
            break;
        }

        const auto* name = callback(index);
        if (name) {
            if (released.length()) {
                released += F(" and ");
            }
            released += FPSTR(name);
        }
    }

    if (released.length()) {
        DEBUG_MSG_P(PSTR("[UART] Port #%zu belongs to %s, %s stopped using it\n"),
            index, String(owner).c_str(), released.c_str());
    } else {
        DEBUG_MSG_P(PSTR("[UART] Port #%zu belongs to %s\n"),
            index, String(owner).c_str());
    }

    return true;
}

#if WEB_SUPPORT
namespace web {

// RELAYS panel: the pins of the first port, what the build gives it and who took one of them (ref. html/src/uart.mjs)
void onConnected(JsonObject& root) {
    JsonObject& pins = root.createNestedObject(F("uartPins"));
    pins[F("tx")] = build::tx(0);
    pins[F("rx")] = build::rx(0);
    pins[F("txUser")] = release::tx_user;
    pins[F("rxUser")] = release::rx_user;

    root[String(FPSTR(settings::keys::TxPin)) + '0'] = settings::tx(0);
    root[String(FPSTR(settings::keys::RxPin)) + '0'] = settings::rx(0);
}

bool onKeyCheck(StringView key, const JsonVariant&) {
    return key.startsWith(settings::Prefix);
}

void setup() {
    wsRegister()
        .onConnected(onConnected)
        .onKeyCheck(onKeyCheck);
}

} // namespace web
#endif

void setup() {
#if TERMINAL_SUPPORT
    terminal::setup();
#endif
    settings::query::setup();
    release::setup();
#if WEB_SUPPORT
    web::setup();
#endif

    bool disable_uart0_rx = false;

    for (size_t index = 0; index < build::PortsMax; ++index) {
        auto& port = internal::ports[index];

        port = make_port(index);
        if (!port) {
            break;
        }

        if (port->type == Type::Uart0 && !port->rx) {
            disable_uart0_rx = true;
        }
    }

    if (disable_uart0_rx) {
        hardware::disable_uart0_rx();
    }
}

} // namespace uart

} // namespace
} // namespace driver
} // namespace espurna

espurna::driver::uart::PortPtr uartPort(size_t index) {
    return espurna::driver::uart::port(index);
}

void uartSetup() {
    espurna::driver::uart::setup();
}

void uartOnClaim(UartClaimRelease callback) {
    espurna::driver::uart::on_claim(callback);
}

bool uartClaim(size_t index, const __FlashStringHelper* owner) {
    return espurna::driver::uart::claim(index, owner);
}

const __FlashStringHelper* uartClaimed(size_t index) {
    return espurna::driver::uart::claimed(index);
}

espurna::driver::uart::PortPtr uartOpen(size_t index) {
    return espurna::driver::uart::open(index);
}

bool uartBaud(size_t index, uint32_t value) {
    return espurna::driver::uart::baud(index, value);
}

#endif // UART_SUPPORT && !UART_MODULE

