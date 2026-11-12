/*

Wire for the BK7231N (LibreTiny has none for the Beken chips, only for LN882H and Realtek): the Arduino TwoWire API,
a software I2C master on any two GPIOs (open drain: a line is driven low or released to its pull-up). A board has the
sensor on whatever pins are soldered, the hardware I2C of the chip is on fixed ones (owner 2026-11-12).

Only the i2c module (ESPurna's i2c.cpp) compiles it (custom_module_libs of platformio_beken.ini): the one Wire of the
board, the I2C drivers and their libraries (VL53L1X, VEML6075, SI1145 ...) are linked against the i2c module.

*/

#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

class TwoWire : public Stream {
public:
    static constexpr size_t Buffer { 128 };

    void begin(int sda, int scl) {
        _sda = sda;
        _scl = scl;
        release(_sda);
        release(_scl);
    }

    // the libraries of the drivers call it again: the pins stay what i2c.cpp gave
    void begin() {
    }

    void end() {
    }

    void setClock(uint32_t frequency) {
        _half = frequency ? std::max<uint32_t>(1, 500000 / frequency) : 5;
    }

    void beginTransmission(uint8_t address) {
        _address = address;
        _size = 0;
    }

    size_t write(uint8_t value) override {
        if (_size >= Buffer) {
            return 0;
        }
        _buffer[_size++] = value;
        return 1;
    }

    size_t write(const uint8_t* data, size_t length) override {
        size_t out = 0;
        while ((out < length) && write(data[out])) {
            ++out;
        }
        return out;
    }

    // 0 - ok, 2 - address NACK, 3 - data NACK, 4 - bus error (Arduino codes)
    uint8_t endTransmission(bool stop = true) {
        if (!start()) {
            return 4;
        }

        uint8_t result = 0;
        if (!writeByte(static_cast<uint8_t>(_address << 1))) {
            result = 2;
        }

        for (size_t index = 0; !result && (index < _size); ++index) {
            if (!writeByte(_buffer[index])) {
                result = 3;
            }
        }

        if (stop || result) {
            this->stop();
        }
        _size = 0;
        return result;
    }

    uint8_t requestFrom(int address, int length) {
        return requestFrom(static_cast<uint8_t>(address), static_cast<uint8_t>(length), true);
    }

    uint8_t requestFrom(uint8_t address, size_t length, bool stop) {
        return requestFrom(address, static_cast<uint8_t>(std::min<size_t>(length, Buffer)), stop);
    }

    uint8_t requestFrom(uint8_t address, uint8_t length, uint8_t stop) {
        return requestFrom(address, length, stop != 0);
    }

    uint8_t requestFrom(uint8_t address, uint8_t length, bool stop = true) {
        _read = 0;
        _size = 0;
        if (length > Buffer) {
            length = Buffer;
        }

        if (!start() || !writeByte(static_cast<uint8_t>((address << 1) | 1))) {
            this->stop();
            return 0;
        }

        for (uint8_t index = 0; index < length; ++index) {
            _buffer[_size++] = readByte((index + 1) < length);
        }

        if (stop) {
            this->stop();
        }
        return _size;
    }

    int available() override {
        return static_cast<int>(_size - _read);
    }

    int read() override {
        return (_read < _size) ? _buffer[_read++] : -1;
    }

    int peek() override {
        return (_read < _size) ? _buffer[_read] : -1;
    }

    void flush() override {
    }

    using Print::write;

private:
    static void release(int pin) {
        pinMode(pin, INPUT_PULLUP);
    }

    static void low(int pin) {
        digitalWrite(pin, LOW);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }

    void wait() const {
        delayMicroseconds(_half);
    }

    // a slave may stretch the clock: SCL is released, then read back until it is high
    bool sclHigh() {
        release(_scl);
        for (int attempt = 0; attempt < 1000; ++attempt) {
            if (digitalRead(_scl)) {
                wait();
                return true;
            }
            delayMicroseconds(1);
        }
        return false;
    }

    bool start() {
        release(_sda);
        if (!sclHigh() || !digitalRead(_sda)) {
            return false;
        }
        low(_sda);
        wait();
        low(_scl);
        wait();
        return true;
    }

    void stop() {
        low(_sda);
        wait();
        sclHigh();
        release(_sda);
        wait();
    }

    bool writeByte(uint8_t value) {
        for (int bit = 7; bit >= 0; --bit) {
            if (value & (1 << bit)) {
                release(_sda);
            } else {
                low(_sda);
            }
            wait();
            if (!sclHigh()) {
                return false;
            }
            low(_scl);
        }

        // ACK: the slave pulls SDA low
        release(_sda);
        wait();
        if (!sclHigh()) {
            return false;
        }
        const bool ack = !digitalRead(_sda);
        low(_scl);
        wait();
        return ack;
    }

    uint8_t readByte(bool ack) {
        uint8_t out = 0;
        release(_sda);
        for (int bit = 7; bit >= 0; --bit) {
            wait();
            sclHigh();
            if (digitalRead(_sda)) {
                out |= (1 << bit);
            }
            low(_scl);
        }

        if (ack) {
            low(_sda);
        } else {
            release(_sda);
        }
        wait();
        sclHigh();
        low(_scl);
        release(_sda);
        wait();
        return out;
    }

    int _sda { -1 };
    int _scl { -1 };
    uint32_t _half { 5 };   // half a clock period, us (100 kHz)
    uint8_t _address { 0 };
    uint8_t _buffer[Buffer] {};
    size_t _size { 0 };
    size_t _read { 0 };
};

extern TwoWire Wire;
