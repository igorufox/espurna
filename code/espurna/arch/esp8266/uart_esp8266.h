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

inline bool& used(int number) {
    static bool ports[2] {false, false};
    return ports[number];
}

inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert) {
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

inline void disable_uart0_rx() {
    ets_isr_mask(1 << ETS_UART_INUM);
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
