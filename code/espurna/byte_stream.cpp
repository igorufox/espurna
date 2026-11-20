/*

ESPurna's own byte stream (byte_stream.h)

*/

#include "byte_stream.h"
#include "system_time.h"

namespace espurna {

size_t ByteStream::read(uint8_t* out, size_t size) {
    size_t done { 0 };
    while ((done < size) && available()) {
        const int value = read();
        if (value < 0) {
            break;
        }

        out[done++] = static_cast<uint8_t>(value);
    }

    return done;
}

int ByteStream::timedRead() {
    int value { -1 };
    time::blockingDelay(
        duration::Milliseconds(_timeout),
        duration::Milliseconds(1),
        [&]() {
            value = read();
            return value < 0;
        });

    return value;
}

size_t ByteStream::readBytes(uint8_t* out, size_t size) {
    size_t done { 0 };
    while (done < size) {
        const int value = timedRead();
        if (value < 0) {
            break;
        }

        out[done++] = static_cast<uint8_t>(value);
    }

    return done;
}

size_t ByteStream::readBytesUntil(char terminator, char* out, size_t size) {
    size_t done { 0 };
    while (done < size) {
        const int value = timedRead();
        if ((value < 0) || (value == terminator)) {
            break;
        }

        out[done++] = static_cast<char>(value);
    }

    return done;
}

} // namespace espurna
