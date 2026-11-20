#pragma once
// Update of the layer: the part of the UpdateClass of the ESP8266 / ESP32 cores that ESPurna calls (ota.cpp), once
// for every chip over core_chip.h (the chip writes its update slot)

#include <Arduino.h>

#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF

class UpdateClass {
public:
    bool begin(size_t size = UPDATE_SIZE_UNKNOWN);
    size_t write(uint8_t* data, size_t len);
    bool end(bool evenIfRemaining = false);
    void abort();

    bool isRunning() const {
        return _running;
    }

    bool hasError() const {
        return _error != nullptr;
    }

    bool isFinished() const {
        return _finished;
    }

    const char* errorString() const {
        return _error ? _error : "No Error";
    }

    void printError(Print& out);

    size_t size() const {
        return _size;
    }

    size_t progress() const {
        return _progress;
    }

    size_t remaining() const {
        return (_size == UPDATE_SIZE_UNKNOWN) ? 0 : (_size - _progress);
    }

    void clearError() {
        _error = nullptr;
    }

private:
    void fail(const char* error);

    const char* _error { nullptr };
    size_t _size { 0 };
    size_t _progress { 0 };
    bool _running { false };
    bool _finished { false };
};

extern UpdateClass Update;
