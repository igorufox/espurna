/*

UART MODULE, LibreTiny part (ref. uart_driver.h)

Pins are fixed, UART1 is P11 (TX) & P10 (RX), same pads are used for flashing.
UART2 is P0 (TX) & P1 (RX), SDK and LibreTiny log there by default.
Unlike esp8266, config is ArduinoCore-API SERIAL_... values. esp8266 pin pairs mean nothing here

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

constexpr uint16_t libretiny_config(Config config) {
    return ((config.data_bits == 5) ? SERIAL_DATA_5 :
            (config.data_bits == 6) ? SERIAL_DATA_6 :
            (config.data_bits == 7) ? SERIAL_DATA_7 : SERIAL_DATA_8)
        | ((config.parity == Parity::Even) ? SERIAL_PARITY_EVEN :
            (config.parity == Parity::Odd) ? SERIAL_PARITY_ODD : SERIAL_PARITY_NONE)
        | ((config.stop_bits == 2) ? SERIAL_STOP_BIT_2 : SERIAL_STOP_BIT_1);
}

// no inverted levels
inline Opened open(uint32_t baudrate, uint8_t tx, uint8_t rx, Config config, bool) {
    const auto pins = [&](uint8_t port_tx, uint8_t port_rx) {
        return ((tx == port_tx) || (tx == GPIO_NONE))
            && ((rx == port_rx) || (rx == GPIO_NONE))
            && ((tx != GPIO_NONE) || (rx != GPIO_NONE));
    };

    const int number =
        pins(11, 10) ? 1 :
        pins(0, 1) ? 2 : -1;
    if (number < 0) {
        return Opened{nullptr, Type::Unknown};
    }

    const int slot = number - 1;
    if (used(slot)) {
        return Opened{nullptr, Type::Unknown};
    }

    used(slot) = true;

    auto* ptr = new SerialClass(number,
        (rx == GPIO_NONE) ? PIN_INVALID : rx,
        (tx == GPIO_NONE) ? PIN_INVALID : tx);
    ptr->begin(baudrate, libretiny_config(config));

    return Opened{ptr, (number == 1) ? Type::Uart0 : Type::Uart1};
}

// SDK owns its log UART
inline void disable_uart0_rx() {
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
