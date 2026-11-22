/*

SPI of the chip (the spi module, spi.cpp). ESP8266 on the RTOS SDK: the HSPI of the SDK's spi driver, its own pins
(SCK GPIO14, MISO GPIO12, MOSI GPIO13)

*/

#pragma once

#include <chip_sdk.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace spi {

// the clock divider of the RFM69 radio (rfm69.cpp, the select() of the RFM69 library): DIV4
constexpr uint32_t Rfm69Divider { 4 }; // SPI_CLOCK_DIV4

constexpr uint8_t DefaultSck { 14 };
constexpr uint8_t DefaultMiso { 12 };
constexpr uint8_t DefaultMosi { 13 };

inline bool begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    return ::espurna::core::chip::spi_begin(sck, miso, mosi);
}

} // namespace spi
} // namespace driver
} // namespace espurna
