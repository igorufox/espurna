/*

ESPurna's own byte stream: a UART port (uart.cpp) and what the drivers on it read and write (sensors, rfbridge, tuya,
nspanel, the serial terminal and log). Instead of the Arduino Stream: espurna::Output to write, a few reads on top.

*/

#pragma once

#include "text_types.h"

namespace espurna {

struct ByteStream : public Output {
    using Output::write;

    // bytes received and not read yet
    virtual size_t available() = 0;
    // the next byte, -1 when none
    virtual int read() = 0;
    // the next byte without taking it, -1 when none
    virtual int peek() = 0;

    // what arrived, up to `size`, without waiting
    virtual size_t read(uint8_t* out, size_t size);

    // what write() takes now without waiting
    virtual size_t availableForWrite() {
        return 0xFFFF;
    }

    // another speed (a hardware port). false - not this stream
    virtual bool baud(uint32_t) {
        return false;
    }

    // readBytes() and timedRead() wait this long for every next byte
    void setTimeout(uint32_t ms) {
        _timeout = ms;
    }

    // `size` bytes, or fewer when nothing came within the timeout
    size_t readBytes(uint8_t* out, size_t size);

    size_t readBytes(char* out, size_t size) {
        return readBytes(reinterpret_cast<uint8_t*>(out), size);
    }

    // bytes up to `terminator` (not stored), at most `size`, within the timeout
    size_t readBytesUntil(char terminator, char* out, size_t size);

    // the next byte, waiting up to the timeout; -1 when none came
    int timedRead();

private:
    uint32_t _timeout { 1000 };
};

} // namespace espurna
