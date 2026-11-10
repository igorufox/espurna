/*

UART MODULE

Chip part of the hardware UART ports, ref. arch/<chip>/uart_<chip>.h. Only uart.cpp includes it,
every function is inline (uart.cpp is also a part of the sensors module, ref. custom_modules)

- hardware::open(baudrate, tx, rx, config, invert)
                         hardware UART on these pins. {nullptr} when the chip has none there,
                         or that UART is already used by another port
- hardware::disable_uart0_rx()
                         no port reads the UART0 RX, its interrupt is not needed

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

#include "uart.h"
#include "libs/BasePin.h"

namespace espurna {
namespace driver {
namespace uart {

enum class Parity {
    None,
    Even,
    Odd,
};

struct Config {
    uint8_t data_bits;
    Parity parity;
    uint8_t stop_bits;
};

namespace hardware {

struct Opened {
    Stream* stream;

    // Uart0 is the UART of the serial console
    Type type;
};

// esp8266 pin pairs. ESP32 takes the same ones, any other pins go to the software port
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

// SerialConfig bits of the esp8266 Core, ESP32 core reads the same ones
constexpr int data_bits_from_config(uint8_t bits) {
    return (bits == 5) ? 0 :
        (bits == 6) ? 0b100 :
        (bits == 7) ? 0b1000 :
        (bits == 8) ? 0b1100 :
            data_bits_from_config(8);
}

constexpr int parity_from_config(Parity parity) {
    return (parity == Parity::None) ? 0 :
        (parity == Parity::Even) ? 0b10 :
        (parity == Parity::Odd) ? 0b11 :
            parity_from_config(Parity::None);
}

constexpr int stop_bits_from_config(uint8_t bits) {
    return (bits == 1) ? 0b10000 :
        (bits == 2) ? 0b110000 :
            stop_bits_from_config(1);
}

constexpr int serial_config(Config config) {
    return data_bits_from_config(config.data_bits)
        | parity_from_config(config.parity)
        | stop_bits_from_config(config.stop_bits);
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna

#if defined(ESP8266)
#include "arch/esp8266/uart_esp8266.h"
#elif defined(ESP32)
#include "arch/esp32/uart_esp32.h"
#elif defined(LIBRETINY)
#include "arch/beken/uart_beken.h"
#endif
