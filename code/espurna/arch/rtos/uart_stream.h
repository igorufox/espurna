/*

UART MODULE: a hardware port of the chip as espurna::ByteStream, on the UART calls of arch/rtos/chip_sdk.h
(ESP8266_RTOS_SDK, ESP-IDF). No Arduino HardwareSerial.

*/

#pragma once

#include "../../byte_stream.h"
#include "chip_sdk.h"

#include "../../system_time.h"

namespace espurna {
namespace driver {
namespace uart {
namespace hardware {

class SdkStream final : public ByteStream {
public:
    explicit SdkStream(int port) :
        _port(port)
    {}

    ~SdkStream() override {
        core::chip::uart_close(_port);
    }

    SdkStream(const SdkStream&) = delete;
    SdkStream& operator=(const SdkStream&) = delete;

    using ByteStream::write;
    using ByteStream::read;

    // everything goes out: waits for room in the TX buffer (as HardwareSerial did)
    void write(const char* data, size_t size) override {
        auto* ptr = reinterpret_cast<const uint8_t*>(data);
        while (size) {
            const auto sent = core::chip::uart_write(_port, ptr, size);
            ptr += sent;
            size -= sent;
            if (size && !sent) {
                time::delay(duration::Milliseconds(1));
            }
        }
    }

    void flush() override {
        core::chip::uart_flush(_port);
    }

    size_t available() override {
        return core::chip::uart_available(_port);
    }

    int read() override {
        uint8_t value;
        return (core::chip::uart_read(_port, &value, 1) == 1) ? value : -1;
    }

    size_t read(uint8_t* out, size_t size) override {
        return core::chip::uart_read(_port, out, size);
    }

    int peek() override {
        return core::chip::uart_peek(_port);
    }

    size_t availableForWrite() override {
        return core::chip::uart_write_space(_port);
    }

    bool baud(uint32_t value) override {
        core::chip::uart_baud(_port, value);
        return true;
    }

private:
    int _port;
};

// SERIAL_* of chip_sdk.h
inline uint16_t sdk_config(uint8_t data_bits, bool even, bool odd, uint8_t stop_bits) {
    namespace serial = core::chip::serial;
    return ((data_bits == 5) ? serial::Data5 :
            (data_bits == 6) ? serial::Data6 :
            (data_bits == 7) ? serial::Data7 : serial::Data8)
        | (even ? serial::ParityEven : odd ? serial::ParityOdd : serial::ParityNone)
        | ((stop_bits == 2) ? serial::StopBit2 : serial::StopBit1);
}

} // namespace hardware
} // namespace uart
} // namespace driver
} // namespace espurna
