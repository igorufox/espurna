/*

SPI for the BK7231N (LibreTiny has none for the Beken chips; the BDK's driver/spi/SPI.h is not the Arduino one): the
Arduino SPIClass API, a software master on any three GPIOs (mode 0..3, MSB or LSB first, the clock from the frequency).
Like the software Wire of lib_beken/Wire: a board has its SPI device on whatever pins are soldered.

Only the spi module (ESPurna's spi.cpp) compiles it (custom_module_libs of platformio_beken.ini): the one SPI of the
board, the modules on SPI (rfm69, mcp23s08) and their libraries are linked against the spi module.

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#ifndef SPI_MODE0
#define SPI_MODE0 0x00
#define SPI_MODE1 0x01
#define SPI_MODE2 0x02
#define SPI_MODE3 0x03
#endif

#ifndef MSBFIRST
#define LSBFIRST 0
#define MSBFIRST 1
#endif

// dividers of a 16 MHz clock, as the AVR core has them (RFM69 sets them)
#define SPI_CLOCK_DIV2 2
#define SPI_CLOCK_DIV4 4
#define SPI_CLOCK_DIV8 8
#define SPI_CLOCK_DIV16 16
#define SPI_CLOCK_DIV32 32
#define SPI_CLOCK_DIV64 64
#define SPI_CLOCK_DIV128 128

class SPISettings {
public:
    SPISettings() = default;
    SPISettings(uint32_t clock, uint8_t bit_order, uint8_t data_mode) :
        clock(clock),
        bit_order(bit_order),
        data_mode(data_mode)
    {}

    uint32_t clock { 1000000 };
    uint8_t bit_order { MSBFIRST };
    uint8_t data_mode { SPI_MODE0 };
};

class SPIClass {
public:
    // the pins of the bus (the spi module from the settings); begin() alone keeps the ones set
    void begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss = -1);
    void begin();
    void end();

    void beginTransaction(const SPISettings& settings);
    void endTransaction();

    void setDataMode(uint8_t mode);
    void setBitOrder(uint8_t order);
    void setClockDivider(uint32_t divider);
    void setFrequency(uint32_t frequency);
    void setHwCs(bool) {
    }

    uint8_t transfer(uint8_t data);
    uint16_t transfer16(uint16_t data);
    void transfer(void* buffer, size_t size);
    void transferBytes(const uint8_t* out, uint8_t* in, size_t size);

private:
    int8_t _sck { -1 };
    int8_t _miso { -1 };
    int8_t _mosi { -1 };
    uint8_t _mode { SPI_MODE0 };
    uint8_t _order { MSBFIRST };
    uint32_t _half { 1 };   // half a clock period, us
};

extern SPIClass SPI;
