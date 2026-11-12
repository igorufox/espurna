/*

UART MODULE, ESP32 part (ref. uart_driver.h)

Any pins are routed through the GPIO matrix. UART0 keeps its default pins (1 & 3, serial
console / debug), UART2 takes any other pins (UART1 default pins are the flash ones).
Unlike esp8266, HardwareSerial::begin() takes (baud, config, rx, tx, invert) and there is no
separate mode or swap(). -1 leaves the pin unused

*/

#pragma once

#include <Arduino.h>

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

    used(slot) = true;

    auto* ptr = new HardwareSerial(number);
    // the buffer is made by begin(): its size first
    if (rx_buffer) {
        ptr->setRxBufferSize(rx_buffer);
    }
    ptr->begin(baudrate,
        static_cast<SerialConfig>(serial_config(config)),
        (rx == GPIO_NONE) ? -1 : static_cast<int8_t>(rx),
        (tx == GPIO_NONE) ? -1 : static_cast<int8_t>(tx),
        invert);

    return Opened{ptr, (number == 0) ? Type::Uart0 : Type::Uart1};
}

// another speed for a hardware port, after what is queued went out
inline bool baud(Stream* stream, Type type, uint32_t value) {
    if ((type != Type::Uart0) && (type != Type::Uart1)) {
        return false;
    }

    auto* serial = static_cast<HardwareSerial*>(stream);
    serial->flush(true);
    serial->updateBaudRate(value);
    return true;
}

// the console UART belongs to the framework, its RX interrupt stays
inline void disable_uart0_rx() {
}

// SDK log goes to the console port of the framework, nothing to switch
inline void sdk_log(Type) {
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
