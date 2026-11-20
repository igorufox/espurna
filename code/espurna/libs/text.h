/*

TEXT - the one escaping and base64 of everything ESPurna builds (the head, code/migration-app). No
Arduino: any string type that takes `+= char` and `+= const char*` (Arduino String, std::string), any byte container
with push_back().

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace espurna {
namespace text {

// A JSON string value, quotes included, that is also a safe JavaScript literal inside <script>: quote and backslash
// escaped, control characters and < > as \uXXXX
template <typename Out>
void jsonString(Out& out, const char* value, size_t len) {
    out += '"';
    for (size_t index = 0; value && (index < len); ++index) {
        const char c = value[index];
        if ((c == '"') || (c == '\\')) {
            out += '\\';
            out += c;
        } else if ((static_cast<unsigned char>(c) < 0x20) || (c == '<') || (c == '>')) {
            // by hand: the migration image's formatter (the SDK's) has no field width
            static const char Digits[] = "0123456789abcdef";
            const auto byte = static_cast<unsigned char>(c);
            const char code[7] { '\\', 'u', '0', '0', Digits[byte >> 4], Digits[byte & 15], '\0' };
            out += static_cast<const char*>(code);
        } else {
            out += c;
        }
    }
    out += '"';
}

template <typename Out>
void jsonString(Out& out, const char* value) {
    jsonString(out, value, value ? strlen(value) : 0);
}

// any string type with c_str() and length()
template <typename Out, typename T, typename = decltype(std::declval<const T&>().c_str())>
void jsonString(Out& out, const T& value) {
    jsonString(out, value.c_str(), value.length());
}

// --- base64 (RFC 4648, with padding) --------------------------------------------------------------------------------

template <typename Out>
void base64Encode(Out& out, const uint8_t* data, size_t size) {
    static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t index = 0; index < size; index += 3) {
        const uint32_t chunk = (static_cast<uint32_t>(data[index]) << 16)
            | ((index + 1 < size) ? (static_cast<uint32_t>(data[index + 1]) << 8) : 0)
            | ((index + 2 < size) ? static_cast<uint32_t>(data[index + 2]) : 0);
        out += Alphabet[(chunk >> 18) & 0x3f];
        out += Alphabet[(chunk >> 12) & 0x3f];
        out += (index + 1 < size) ? Alphabet[(chunk >> 6) & 0x3f] : '=';
        out += (index + 2 < size) ? Alphabet[chunk & 0x3f] : '=';
    }
}

// into `out` (cleared first, push_back of each byte); padding and white space are skipped. false - a foreign
// character or nothing decoded
template <typename Bytes>
bool base64Decode(Bytes& out, const char* in, size_t size) {
    out.clear();
    uint32_t buffer { 0 };
    int bits { 0 };
    for (size_t index = 0; index < size; ++index) {
        const char c = in[index];
        int value;
        if ((c >= 'A') && (c <= 'Z')) {
            value = c - 'A';
        } else if ((c >= 'a') && (c <= 'z')) {
            value = c - 'a' + 26;
        } else if ((c >= '0') && (c <= '9')) {
            value = c - '0' + 52;
        } else if (c == '+') {
            value = 62;
        } else if (c == '/') {
            value = 63;
        } else if ((c == '=') || (c == '\r') || (c == '\n') || (c == ' ') || (c == '\t')) {
            continue;
        } else {
            return false;
        }

        buffer = ((buffer << 6) | static_cast<uint32_t>(value)) & 0xffffff;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
        }
    }

    return out.size() > 0;
}

} // namespace text
} // namespace espurna
