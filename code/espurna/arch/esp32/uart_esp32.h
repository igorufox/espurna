/*

UART MODULE, ESP32 part (ref. uart_driver.h)

Any pins are routed through the GPIO matrix. UART0 keeps its default pins (1 & 3, serial
console / debug), UART2 is used for everything else (UART1 default pins are the flash ones).
Unlike esp8266, HardwareSerial::begin() takes (baud, config, rx, tx, invert) and there is no
separate mode or swap(). -1 leaves the pin unused. Only the esp8266 pin pairs are taken,
any other pins go to the software port

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

inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool invert) {
    if (!uart0_normal(tx, rx) && !uart0_swapped(tx, rx) && !uart1_normal(tx, rx)) {
        return Opened{nullptr, Type::Unknown};
    }

    const int number = uart0_normal(tx, rx) ? 0 : 2;
    const int slot = (number == 0) ? 0 : 1;
    if (used(slot)) {
        return Opened{nullptr, Type::Unknown};
    }

    used(slot) = true;

    auto* ptr = new HardwareSerial(number);
    ptr->begin(baudrate,
        static_cast<SerialConfig>(serial_config(config)),
        (rx == GPIO_NONE) ? -1 : static_cast<int8_t>(rx),
        (tx == GPIO_NONE) ? -1 : static_cast<int8_t>(tx),
        invert);

    return Opened{ptr, (number == 0) ? Type::Uart0 : Type::Uart1};
}

// the console UART belongs to the framework, its RX interrupt stays
inline void disable_uart0_rx() {
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
