/*

UART MODULE, BK72xx part (ref. uart_driver.h)

Pins are fixed, UART1 is P11 (TX) & P10 (RX), same pads are used for flashing.
UART2 is P0 (TX) & P1 (RX).
esp8266 pin pairs mean nothing here.
The port is SdkStream (arch/rtos/uart_stream.h, the same as the other chips) on the UART calls of the chip (core_beken.cpp
uart_*, the BDK's UART driver with a receive ring buffer)

*/

#pragma once

#include "../rtos/uart_stream.h"

namespace espurna {
namespace driver {
namespace uart {
namespace hardware {

inline bool& used(int slot) {
    static bool ports[2] {false, false};
    return ports[slot];
}

// no inverted levels
// the receive ring buffer of core_beken.cpp has a fixed size: rx_buffer is not taken
// the UART of these pins, -1 when the chip has none there
inline int number(uint8_t tx, uint8_t rx) {
    const auto pins = [&](uint8_t port_tx, uint8_t port_rx) {
        return ((tx == port_tx) || (tx == GPIO_NONE))
            && ((rx == port_rx) || (rx == GPIO_NONE))
            && ((tx != GPIO_NONE) || (rx != GPIO_NONE));
    };

    return pins(11, 10) ? 1 :
        pins(0, 1) ? 2 : -1;
}

inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert, size_t rx_buffer) {
    const int number = hardware::number(tx, rx);
    if (number < 0) {
        return Opened{nullptr, Type::Unknown};
    }

    const int slot = number - 1;
    if (used(slot)) {
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

    used(slot) = true;
    return Opened{new SdkStream(port), (number == 1) ? Type::Uart0 : Type::Uart1};
}

// another speed for a hardware port, after what is queued went out
inline bool baud(ByteStream* stream, Type type, uint32_t value) {
    if ((type != Type::Uart0) && (type != Type::Uart1)) {
        return false;
    }

    return stream->baud(value);
}

// SDK owns its log UART
inline void disable_uart0_rx() {
}

// SDK log goes to its own UART, nothing to switch
inline void sdk_log(Type) {
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
