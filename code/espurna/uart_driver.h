/*

UART MODULE

Chip part of the hardware UART ports, ref. arch/<chip>/uart_<chip>.h. Only uart.cpp includes it,
every function is inline (uart.cpp is also a part of the sensors module, ref. custom_modules)

- hardware::open(baudrate, tx, rx, config, invert, rx_buffer)
                         hardware UART on these pins. {nullptr} when the chip has none there,
                         or that UART is already used by another port (uart.cpp tries the software port then).
                         Which pins: esp8266 - its pin pairs, ESP32 - any pins (GPIO matrix), LibreTiny - fixed pins
- hardware::baud(stream, type, value)
                         another speed for an open hardware port (false for a software port)
- hardware::disable_uart0_rx()
                         no port reads the UART0 RX, its interrupt is not needed
- hardware::sdk_log(type)
                         SDK log output to this port (Uart0 / Uart1), Type::Unknown turns it off.
                         Only the esp8266 SDK writes its log through our ports

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
