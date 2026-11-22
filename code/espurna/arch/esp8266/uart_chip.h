/*

UART MODULE, ESP8266 on ESP8266_RTOS_SDK (ref. uart_driver.h)

UART0 on GPIO1 (TX) / GPIO3 (RX), the console, or swapped to GPIO15 (TX) / GPIO13 (RX); UART1 transmits only, on GPIO2.
The port is SdkStream (arch/rtos/uart_stream.h) on the UART calls of the chip (core_esp8266.cpp uart_*, the SDK's
UART driver); the pins pick the port

*/

#pragma once

#include "../rtos/uart_stream.h"

namespace espurna {
namespace driver {
namespace uart {
namespace hardware {

inline bool& used(int number) {
    static bool ports[2] {false, false};
    return ports[number];
}

// UART0 on its own pins or the swapped ones
constexpr bool uart0_pins(uint8_t tx, uint8_t rx) {
    return (((tx == 1) || (tx == GPIO_NONE)) && ((rx == 3) || (rx == GPIO_NONE))
            && ((tx != GPIO_NONE) || (rx != GPIO_NONE)))
        || (((tx == 15) || (tx == GPIO_NONE)) && ((rx == 13) || (rx == GPIO_NONE))
            && ((tx != GPIO_NONE) || (rx != GPIO_NONE)));
}

// UART1: TX on GPIO2, nothing to receive with
constexpr bool uart1_pins(uint8_t tx, uint8_t rx) {
    return (tx == 2) && (rx == GPIO_NONE);
}

// the UART of these pins, -1 when the chip has none there
inline int number(uint8_t tx, uint8_t rx) {
    return uart0_pins(tx, rx)
            ? 0 :
        uart1_pins(tx, rx)
            ? 1
            : -1;
}

inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert, size_t rx_buffer) {
    const int number = hardware::number(tx, rx);
    if ((number < 0) || used(number)) {
        return Opened{nullptr, Type::Unknown};
    }

    const auto pin = [](uint8_t value) {
        return (value == GPIO_NONE) ? core::chip::NoPin : value;
    };

    const auto port = core::chip::uart_open(baudrate,
        sdk_config(config.data_bits, config.parity == Parity::Even, config.parity == Parity::Odd, config.stop_bits),
        pin(rx), pin(tx), invert, rx_buffer);
    if (port < 0) {
        return Opened{nullptr, Type::Unknown};
    }

    used(number) = true;
    return Opened{new SdkStream(port), (number == 0) ? Type::Uart0 : Type::Uart1};
}

// another speed for a hardware port, after what is queued went out
inline bool baud(ByteStream* stream, Type type, uint32_t value) {
    if ((type != Type::Uart0) && (type != Type::Uart1)) {
        return false;
    }

    return stream->baud(value);
}

// the console UART keeps its RX
inline void disable_uart0_rx() {
}

// SDK log goes to the console port, nothing to switch
inline void sdk_log(Type) {
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
