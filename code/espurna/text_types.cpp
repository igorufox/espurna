/*

ESPurna's own text types (text_types.h)

*/

#include "text_types.h"
#include "ip4.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <utility>

namespace espurna {

size_t Output::vprintf(const char* format, va_list args) {
    char buffer[128];

    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);

    if (size < 0) {
        return 0;
    }

    if (static_cast<size_t>(size) < sizeof(buffer)) {
        write(buffer, static_cast<size_t>(size));
        return static_cast<size_t>(size);
    }

    auto* data = static_cast<char*>(std::malloc(size + 1));
    if (!data) {
        return 0;
    }

    std::vsnprintf(data, size + 1, format, args);
    write(data, static_cast<size_t>(size));
    std::free(data);

    return static_cast<size_t>(size);
}

size_t Output::printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const auto out = vprintf(format, args);
    va_end(args);
    return out;
}

bool Ip4::fromString(const char* text) {
    if (!text) {
        return false;
    }

    uint32_t out { 0 };
    for (int octet = 0; octet < 4; ++octet) {
        if ((*text < '0') || (*text > '9')) {
            return false;
        }

        uint32_t value { 0 };
        int digits { 0 };
        while ((*text >= '0') && (*text <= '9')) {
            value = (value * 10) + (*text - '0');
            ++text;
            if ((++digits > 3) || (value > 255)) {
                return false;
            }
        }

        out |= value << (8 * octet);
        if (octet < 3) {
            if (*text != '.') {
                return false;
            }
            ++text;
        }
    }

    if (*text != '\0') {
        return false;
    }

    _value = out;
    return true;
}

size_t Ip4::toString(char* out, size_t size) const {
    const int length = std::snprintf(out, size, "%u.%u.%u.%u",
        (*this)[0], (*this)[1], (*this)[2], (*this)[3]);
    return (length > 0) ? static_cast<size_t>(length) : 0;
}

} // namespace espurna
