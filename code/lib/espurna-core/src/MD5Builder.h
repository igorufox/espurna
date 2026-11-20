#pragma once
// MD5Builder of the ESP8266 / ESP32 cores (FauxmoESP: the Hue id of a device), on the MD5 of mbedTLS, the one crypto
// library of ESPurna on every chip (owner 11-18), mbedTLS 3.6.6 on all three (code/libraries.ini)

#include <Arduino.h>
#include <mbedtls/md5.h>

class MD5Builder {
public:
    MD5Builder() {
        mbedtls_md5_init(&_context);
    }

    ~MD5Builder() {
        mbedtls_md5_free(&_context);
    }

    MD5Builder(const MD5Builder&) = delete;
    MD5Builder& operator=(const MD5Builder&) = delete;

    void begin() {
        mbedtls_md5_starts(&_context);
    }

    void add(const uint8_t* data, size_t size) {
        mbedtls_md5_update(&_context, data, size);
    }

    void add(const char* text) {
        add(reinterpret_cast<const uint8_t*>(text), strlen(text));
    }

    void add(const String& text) {
        add(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
    }

    void calculate() {
        mbedtls_md5_finish(&_context, _digest);
    }

    void getBytes(uint8_t* out) const {
        memcpy(out, _digest, sizeof(_digest));
    }

    void getChars(char* out) const {
        for (size_t index = 0; index < sizeof(_digest); ++index) {
            snprintf(out + (index * 2), 3, "%02x", _digest[index]);
        }
    }

    String toString() const {
        char out[(sizeof(_digest) * 2) + 1];
        getChars(out);
        return String(out);
    }

private:
    mbedtls_md5_context _context;
    uint8_t _digest[16];
};
