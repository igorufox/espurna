#pragma once
// Serial of the layer: one class for every hardware UART of every chip, over core_chip.h
#pragma once

#include "arduino-api/HardwareSerial.h"

#include <stddef.h>
#include <stdint.h>

class HardwareSerialPort : public arduino::HardwareSerial {
public:
    static constexpr uint8_t NoPin { 0xff };
    static constexpr size_t DefaultRxBuffer { 256 };

    HardwareSerialPort() = default;

    // begin(baud) on the console UART and its default pins
    void begin(unsigned long baud) override {
        begin(baud, SERIAL_8N1);
    }

    void begin(unsigned long baud, uint16_t config) override {
        begin(baud, config, NoPin, NoPin, false, DefaultRxBuffer);
    }

    // ESP32 core order: begin(baud, config, rx, tx, invert)
    bool begin(unsigned long baud, uint16_t config, uint8_t rx, uint8_t tx,
            bool invert = false, size_t rx_buffer = DefaultRxBuffer);

    void end() override;
    void updateBaudRate(unsigned long baud);

    int available() override;
    int peek() override;
    int read() override;
    size_t read(uint8_t* buffer, size_t size);
    int availableForWrite() override;
    void flush() override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    using Print::write;

    operator bool() override {
        return _port >= 0;
    }

    int port() const {
        return _port;
    }

private:
    int _port { -1 };
};

// The console UART (debug output, terminal over serial)
extern HardwareSerialPort Serial;
