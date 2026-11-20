// ltoa() / ultoa() of the ESP cores, newlib has only itoa() / utoa(); weak: a C library that has them wins.
// A file of their own, not wiring.cpp: that object shares millis() / pinMode() ... with the head, the module link takes
// such a member for the head's and leaves it out of the arduino module, and these two went missing with it (Beken
// 20.11: exported from a head that never has them, every build again)

#include <stddef.h>

extern "C" {

static char* unsigned_to(unsigned long value, char* out, int radix, bool negative) {
    if ((radix < 2) || (radix > 36)) {
        *out = '\0';
        return out;
    }

    char buffer[sizeof(unsigned long) * 8 + 1];
    size_t len = 0;
    do {
        const auto digit = static_cast<char>(value % static_cast<unsigned long>(radix));
        buffer[len++] = (digit < 10) ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10);
        value /= static_cast<unsigned long>(radix);
    } while (value);

    char* ptr = out;
    if (negative) {
        *ptr++ = '-';
    }

    while (len) {
        *ptr++ = buffer[--len];
    }

    *ptr = '\0';
    return out;
}

__attribute__((weak)) char* ultoa(unsigned long value, char* out, int radix) {
    return unsigned_to(value, out, radix, false);
}

__attribute__((weak)) char* ltoa(long value, char* out, int radix) {
    if ((radix == 10) && (value < 0)) {
        return unsigned_to(0UL - static_cast<unsigned long>(value), out, radix, true);
    }

    return unsigned_to(static_cast<unsigned long>(value), out, radix, false);
}

} // extern "C"
