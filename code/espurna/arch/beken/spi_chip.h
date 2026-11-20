/*

SPI of the chip (the spi module, spi.cpp). Beken: the software SPI of lib_beken/SPI on any three GPIOs, none by
default (a board has its SPI device wherever it is soldered)

*/

#pragma once

#include <SPI.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace spi {

// the clock divider of the RFM69 radio (rfm69.cpp, the select() of the RFM69 library): DIV16, what the library takes on ARM chips (a software SPI here anyway)
constexpr uint32_t Rfm69Divider { SPI_CLOCK_DIV16 };

constexpr uint8_t DefaultSck { 0xff };
constexpr uint8_t DefaultMiso { 0xff };
constexpr uint8_t DefaultMosi { 0xff };

inline bool begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    SPI.begin(sck, miso, mosi);
    return true;
}

} // namespace spi
} // namespace driver
} // namespace espurna
