/*

UART MODULE, ESP32 part (ref. uart_driver.h)

Any pins are routed through the GPIO matrix. UART0 keeps its default pins (1 & 3, serial
console / debug), UART2 takes any other pins (UART1 default pins are the flash ones).
The port is SdkStream (arch/rtos/uart_stream.h, the same as the ESP8266) on the UART calls of the chip (core_esp32.cpp
uart_*, ESP-IDF's UART driver): console pins or none - UART0, any other pins - UART2. No Arduino HardwareSerial

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

// UART0 on its own pins, the serial console
constexpr bool uart0_console(uint8_t tx, uint8_t rx) {
    return ((tx == 1) || (tx == GPIO_NONE))
        && ((rx == 3) || (rx == GPIO_NONE))
        && ((tx != GPIO_NONE) || (rx != GPIO_NONE));
}

// UART2 on the pins of the GPIO matrix, never the console ones
inline bool uart2_pins(uint8_t tx, uint8_t rx) {
    const auto console = [](uint8_t pin) {
        return (pin == 1) || (pin == 3);
    };

    if ((tx == GPIO_NONE) && (rx == GPIO_NONE)) {
        return false;
    }

    if ((tx != GPIO_NONE) && (console(tx) || !gpioValidForOutput(tx))) {
        return false;
    }

    if ((rx != GPIO_NONE) && (console(rx) || !gpioValid(rx))) {
        return false;
    }

    return true;
}

inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert, size_t rx_buffer) {
    const int number =
        uart0_console(tx, rx)
            ? 0 :
        uart2_pins(tx, rx)
            ? 2
            : -1;
    if (number < 0) {
        return Opened{nullptr, Type::Unknown};
    }

    const int slot = (number == 0) ? 0 : 1;
    if (used(slot)) {
        return Opened{nullptr, Type::Unknown};
    }

    const auto pin = [](uint8_t value) {
        return (value == GPIO_NONE) ? core::chip::NoPin : value;
    };

    // rx_buffer 0: the driver's least, twice the FIFO
    const auto port = core::chip::uart_open(baudrate,
        sdk_config(config.data_bits, config.parity == Parity::Even, config.parity == Parity::Odd, config.stop_bits),
        pin(rx), pin(tx), invert, rx_buffer);
    if (port < 0) {
        return Opened{nullptr, Type::Unknown};
    }

    used(slot) = true;
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
