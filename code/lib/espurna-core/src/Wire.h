#pragma once
// Wire (I2C master) of the layer, one for every chip over core_chip.h.
// begin(sda, scl, frequency) / pins() of the ESP32 and ESP8266 cores; no slave mode (nothing uses it)
#pragma once

#include <Arduino.h>
#include "arduino-api/HardwareI2C.h"

// the buffer of a transfer, the cores' name (Adafruit BusIO splits its transfers by it)
#define I2C_BUFFER_LENGTH 128

class TwoWire : public arduino::HardwareI2C {
public:
    static constexpr size_t BufferSize { I2C_BUFFER_LENGTH };
    static constexpr uint32_t DefaultFrequency { 100000 };

    void begin() override;
    void begin(uint8_t) override {
        begin();
    }

    bool begin(int sda, int scl, uint32_t frequency = 0);
    void pins(int sda, int scl);
    void end() override;
    void setClock(uint32_t frequency) override;

    void beginTransmission(uint8_t address) override;
    void beginTransmission(int address) {
        beginTransmission(static_cast<uint8_t>(address));
    }

    uint8_t endTransmission(bool stop) override;
    uint8_t endTransmission() override {
        return endTransmission(true);
    }

    size_t requestFrom(uint8_t address, size_t size, bool stop) override;
    size_t requestFrom(uint8_t address, size_t size) override {
        return requestFrom(address, size, true);
    }

    uint8_t requestFrom(int address, int size) {
        return static_cast<uint8_t>(requestFrom(static_cast<uint8_t>(address), static_cast<size_t>(size), true));
    }

    uint8_t requestFrom(int address, int size, int stop) {
        return static_cast<uint8_t>(requestFrom(static_cast<uint8_t>(address), static_cast<size_t>(size), stop != 0));
    }

    size_t write(uint8_t value) override;
    size_t write(const uint8_t* data, size_t size) override;
    using Print::write;

    int available() override;
    int read() override;
    int peek() override;
    void flush() override;

    void onReceive(void (*)(int)) override {
    }

    void onRequest(void (*)(void)) override {
    }

private:
    int _sda { -1 };
    int _scl { -1 };
    uint32_t _frequency { DefaultFrequency };
    bool _started { false };

    uint8_t _address { 0 };
    bool _transmitting { false };

    uint8_t _tx[BufferSize];
    size_t _tx_size { 0 };

    uint8_t _rx[BufferSize];
    size_t _rx_size { 0 };
    size_t _rx_index { 0 };
};

extern TwoWire Wire;

// the second bus of Arduino-ESP32 (SSD1306Wire names it): ESPurna has one I2C bus (the i2c module), it is that one
extern TwoWire& Wire1;
