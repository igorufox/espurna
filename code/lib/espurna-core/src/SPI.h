#pragma once
// SPI (master) of the layer, one for every chip over core_chip.h.
// The libraries hold `SPIClass*` and call the ESP8266 / ESP32 core methods through it (RFM69: setDataMode,
// setClockDivider; Adafruit BusIO: transferBytes), so SPIClass here is the full class. arduino-api/HardwareSPI.h has
// `typedef HardwareSPI SPIClass` in namespace arduino, which `using namespace arduino` would make ambiguous:
// it is renamed while the header is read (the arduino-api/ copy stays unchanged).
#pragma once

#include <Arduino.h>

#define SPIClass ArduinoApiSPIClass
#include "arduino-api/HardwareSPI.h"
#undef SPIClass

// The divider of the AVR's 16 MHz, as the ESP8266 / ESP32 cores keep it: DIV2 = 8 MHz ... DIV128 = 125 kHz
#define SPI_CLOCK_DIV2   2
#define SPI_CLOCK_DIV4   4
#define SPI_CLOCK_DIV8   8
#define SPI_CLOCK_DIV16  16
#define SPI_CLOCK_DIV32  32
#define SPI_CLOCK_DIV64  64
#define SPI_CLOCK_DIV128 128

class SPIClass : public arduino::HardwareSPI {
public:
    static constexpr uint32_t BaseClock { 16000000 };

    void begin() override;
    bool begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss = -1);
    void pins(int8_t sck, int8_t miso, int8_t mosi, int8_t ss);
    void end() override;

    void beginTransaction(arduino::SPISettings settings) override;
    void endTransaction() override;

    void setFrequency(uint32_t frequency);
    void setClockDivider(uint32_t divider);
    void setDataMode(uint8_t mode);
    void setBitOrder(uint8_t order);

    uint8_t transfer(uint8_t data) override;
    uint16_t transfer16(uint16_t data) override;
    void transfer(void* buffer, size_t size) override;
    void transferBytes(const uint8_t* out, uint8_t* in, uint32_t size);
    void writeBytes(const uint8_t* data, uint32_t size);

    void usingInterrupt(int) override {
    }

    void notUsingInterrupt(int) override {
    }

    void attachInterrupt() override {
    }

    void detachInterrupt() override {
    }

private:
    void apply();

    int8_t _sck { -1 };
    int8_t _miso { -1 };
    int8_t _mosi { -1 };
    bool _started { false };

    uint32_t _frequency { 1000000 };
    uint8_t _mode { 0 };
    bool _msb_first { true };
};

extern SPIClass SPI;
