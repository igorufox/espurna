/*

SOFTWARE I2C MASTER

The part of the Arduino TwoWire API i2c.cpp uses, bit-banged on two GPIOs (open drain: a line is driven low or
released to its pull-up). For the frameworks without a Wire library of their own (LibreTiny on the BK7231N);
the others keep their hardware Wire.

*/

#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace espurna {
namespace i2c {

class SoftWire {
public:
    static constexpr size_t Buffer { 128 };

    void begin(int sda, int scl) {
        _sda = sda;
        _scl = scl;
        release(_sda);
        release(_scl);
    }

    void setClock(uint32_t frequency) {
        _half = frequency ? std::max<uint32_t>(1, 500000 / frequency) : 5;
    }

    void beginTransmission(uint8_t address) {
        _address = address;
        _size = 0;
    }

    size_t write(uint8_t value) {
        if (_size >= Buffer) {
            return 0;
        }
        _buffer[_size++] = value;
        return 1;
    }

    size_t write(const uint8_t* data, size_t length) {
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

    int available() const {
        return static_cast<int>(_size - _read);
    }

    int read() {
        return (_read < _size) ? _buffer[_read++] : -1;
    }

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

} // namespace i2c
} // namespace espurna
