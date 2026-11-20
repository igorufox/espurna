#pragma once
// StreamString of the ESP8266 / ESP32 cores: a String that is a Stream too (written with print(), read back),
// tuya.cpp and nspanel_web.cpp build their output with it

#include <Arduino.h>

class StreamString : public Stream, public String {
public:
    StreamString() = default;

    size_t write(const uint8_t* data, size_t size) override {
        return concat(reinterpret_cast<const char*>(data), size) ? size : 0;
    }

    size_t write(uint8_t data) override {
        return concat(static_cast<char>(data)) ? 1 : 0;
    }

    using Print::write;

    int available() override {
        return static_cast<int>(length() - _index);
    }

    int read() override {
        return (_index < length())
            ? static_cast<unsigned char>(charAt(_index++))
            : -1;
    }

    int peek() override {
        return (_index < length())
            ? static_cast<unsigned char>(charAt(_index))
            : -1;
    }

    void flush() override {
    }

private:
    unsigned int _index { 0 };
};
