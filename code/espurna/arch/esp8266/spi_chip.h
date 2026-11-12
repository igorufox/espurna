/*

SPI of the chip (the spi module, spi.cpp). ESP8266: the hardware HSPI, its pins fixed by the chip (SCK GPIO14,
MISO GPIO12, MOSI GPIO13); SPI.pins() takes only those (or the flash pins of the overlap mode)

*/

#pragma once

#include <Arduino.h>
#include <SPI.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace spi {

constexpr uint8_t DefaultSck { 14 };
constexpr uint8_t DefaultMiso { 12 };
constexpr uint8_t DefaultMosi { 13 };

inline bool begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    if (!SPI.pins(sck, miso, mosi, 15)) {
        return false;
    }

    SPI.begin();
    return true;
}

} // namespace spi
} // namespace driver
} // namespace espurna
