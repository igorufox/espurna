/*

SPI of the chip (the spi module, spi.cpp). Beken: the software SPI of lib_beken/SPI on any three GPIOs, none by
default (a board has its SPI device wherever it is soldered)

*/

#pragma once

#include <Arduino.h>
#include <SPI.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace spi {

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
