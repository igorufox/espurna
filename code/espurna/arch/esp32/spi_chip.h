/*

SPI of the chip (the spi module, spi.cpp). ESP32: the hardware SPI on any pins (the GPIO matrix), VSPI's by default

*/

#pragma once

#include <SPI.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace spi {

// the clock divider of the RFM69 radio (rfm69.cpp, the select() of the RFM69 library): DIV4
constexpr uint32_t Rfm69Divider { SPI_CLOCK_DIV4 };

constexpr uint8_t DefaultSck { 18 };
constexpr uint8_t DefaultMiso { 19 };
constexpr uint8_t DefaultMosi { 23 };

inline bool begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    return SPI.begin(sck, miso, mosi, -1);
}

} // namespace spi
} // namespace driver
} // namespace espurna
