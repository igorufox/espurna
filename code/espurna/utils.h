/*

UTILS MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once

#include "byte_stream.h"
#include "text_types.h"

#include "ip4.h"

#include "types.h"

espurna::string prettyDuration(espurna::duration::Seconds);


char* strnstr(const char* buffer, const char* token, size_t n);
bool isNumber(espurna::StringView);

double roundTo(double num, unsigned char positions);
bool almostEqual(double lhs, double rhs, int ulp);
bool almostEqual(double lhs, double rhs);

struct ParseUnsignedResult {
    bool ok { false };
    uint32_t value { 0 };
};

ParseUnsignedResult parseUnsigned(espurna::StringView, int base);
ParseUnsignedResult parseUnsigned(espurna::StringView);
espurna::string formatUnsigned(uint32_t value, int base);

// "-12.50" (espurna::formatDecimal(), no printf("%f"))
inline espurna::string formatDecimalString(double value, unsigned char decimals) {
    char buffer[24];
    espurna::formatDecimal(value, decimals, buffer, sizeof(buffer));
    return espurna::string(buffer);
}

// Same as espurna::Ip4::toString(), which ArduinoCore-API (LibreTiny) does not have.
// LibreTiny provides it (wiring_compat.h)
espurna::string ipToString(const espurna::Ip4&);

// Same as espurna::string::equalsConstantTime(), which ArduinoCore-API (LibreTiny) does not have
bool equalsConstantTime(const espurna::string& lhs, const espurna::string& rhs);

long adjustNumber(long, espurna::StringView operation);

// Stream peek buffer API (hasPeekBufferAPI() / peekConsume()) is only in some cores (esp8266 one). Selected by
// whether the Stream class has it, not by the chip
namespace espurna {
namespace stream {

template <typename T>
inline auto peek_buffer(T& stream, int) -> decltype(stream.hasPeekBufferAPI()) {
    return stream.hasPeekBufferAPI();
}

template <typename T>
inline bool peek_buffer(T&, long) {
    return false;
}

template <typename T>
inline auto peek_consume(T& stream, size_t size, int) -> decltype(stream.peekConsume(size), bool()) {
    if (stream.hasPeekBufferAPI()) {
        stream.peekConsume(size);
        return true;
    }

    return false;
}

template <typename T>
inline bool peek_consume(T&, size_t, long) {
    return false;
}

} // namespace stream
} // namespace espurna

// Stream has the peek buffer and its data can be dropped without copying
inline bool streamPeekBuffer(::espurna::ByteStream& stream) {
    return espurna::stream::peek_buffer(stream, 0);
}

// Drop `size` bytes from the peek buffer. false when there is none, data has to be read instead
inline bool streamPeekConsume(::espurna::ByteStream& stream, size_t size) {
    return espurna::stream::peek_consume(stream, size, 0);
}

char* hexEncode(const uint8_t* in_begin, const uint8_t* in_end, char* out_begin, char* out_end);
size_t hexEncode(const uint8_t* in, size_t in_size, char* out, size_t out_size);
espurna::string hexEncode(const uint8_t* begin, const uint8_t* end);

template <size_t Size>
inline espurna::string hexEncode(const uint8_t (&data)[Size]) {
    return hexEncode(std::begin(data), std::end(data));
}

template <size_t Size>
inline espurna::string hexEncode(const std::array<uint8_t, Size>& data) {
    return hexEncode(data.data(), data.data() + data.size());
}

inline espurna::string hexEncode(espurna::Span<const uint8_t> data) {
    return hexEncode(std::begin(data), std::end(data));
}

inline espurna::string hexEncode(espurna::Span<uint8_t> data) {
    return hexEncode(std::begin(data), std::end(data));
}

inline espurna::string hexEncode(uint8_t value) {
    uint8_t buffer[1] { value };
    return hexEncode(buffer);
}

uint8_t* hexDecode(const char* in_begin, const char* in_end, uint8_t* out_begin, uint8_t* out_end);
size_t hexDecode(const char* in, size_t in_size, uint8_t* out, size_t out_size);

espurna::StringView tryFirstPath(espurna::StringView);

bool tryParseId(espurna::StringView, size_t limit, size_t& out);
bool tryParseIdPath(espurna::StringView, size_t limit, size_t& out);

espurna::StringView stripNewline(espurna::StringView);

size_t consumeAvailable(::espurna::ByteStream&);

