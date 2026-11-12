/*

UART MODULE, esp8266 part (ref. uart_driver.h)

UART0 on the console pins (1 & 3) or swapped (15 & 13), UART1 is TX-only on pin 2

*/

#pragma once

#include <Arduino.h>

namespace espurna {
namespace driver {
namespace uart {
namespace hardware {

// todo; technically, tx==2 is also possible
// but, we reserve that for the uart1 TX-only interface
constexpr bool uart0_normal(uint8_t tx, uint8_t rx) {
    return ((tx == 1) && (rx == 3))
        || ((tx == GPIO_NONE) && (rx == 3))
        || ((tx == 1) && (rx == GPIO_NONE));
}

constexpr bool uart0_swapped(uint8_t tx, uint8_t rx) {
    return ((tx == 15) && (rx == 13))
        || ((tx == GPIO_NONE) && (rx == 13))
        || ((tx == 15) && (rx == GPIO_NONE));
}

constexpr bool uart1_normal(uint8_t tx, uint8_t rx) {
    return (tx == 2) && (rx == GPIO_NONE);
}

inline bool& used(int number) {
    static bool ports[2] {false, false};
    return ports[number];
}

inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert, size_t rx_buffer) {
    const int number =
        uart0_normal(tx, rx)
            ? 0 :
        uart0_swapped(tx, rx)
            ? 0 :
        uart1_normal(tx, rx)
            ? 1
            : -1;

    if ((number < 0) || used(number)) {
        return Opened{nullptr, Type::Unknown};
    }

    const int mode =
        (tx == GPIO_NONE)
            ? SERIAL_RX_ONLY :
        (rx == GPIO_NONE)
            ? SERIAL_TX_ONLY :
        ((tx != GPIO_NONE) && (rx != GPIO_NONE))
            ? SERIAL_FULL
            : -1;
    if (mode < 0) {
        return Opened{nullptr, Type::Unknown};
    }

    used(number) = true;

    auto* ptr = new HardwareSerial(number);
    if (rx_buffer) {
        ptr->setRxBufferSize(rx_buffer);
    }
    ptr->begin(baudrate,
        static_cast<SerialConfig>(serial_config(config)),
        static_cast<SerialMode>(mode),
        tx, invert);
    if ((number == 0) && (uart0_swapped(tx, rx))) {
        ptr->flush();
        ptr->swap();
    }

    return Opened{ptr, (number == 0) ? Type::Uart0 : Type::Uart1};
}

// another speed for a hardware port
inline bool baud(Stream* stream, Type type, uint32_t value) {
    if ((type != Type::Uart0) && (type != Type::Uart1)) {
        return false;
    }

    auto* serial = static_cast<HardwareSerial*>(stream);
    serial->flush();
    serial->updateBaudRate(value);
    return true;
}

inline void disable_uart0_rx() {
    ets_isr_mask(1 << ETS_UART_INUM);
}

inline void sdk_log(Type type) {
    switch (type) {
    case Type::Uart0:
        uart_set_debug(0);
        break;
    case Type::Uart1:
        uart_set_debug(1);
        break;
    default:
        uart_set_debug(UART_NO);
        break;
    }
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
