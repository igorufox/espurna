/*

The standard output of the ESP8266 build: printf, puts, putchar and assert of the SDK's code (debug prints of lwIP's
DHCP server, the ring buffer, the log's line ends) through the SDK's own ets_vprintf to the console UART, as the SDK's
logs go. The SDK's stdio streams over its VFS UART driver are not opened (components/newlib/src/reent_init.c of the SDK
package, ESPURNA_NO_STDIO): two ways to the same UART, the newlib FILE layer and the VFS cost about 10 KB of the head.

*/

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <esp_libc.h>

/*

The formatted output of the ESP8266 build: vsnprintf and the rest of its family, own (regress-todo.md item 20, 18.11).
The newlib of the toolchain is 'nano' (sdkconfig.defaults): its printf has no C99 sizes (%zu, %lld: the RELAY command
crashed the board) and prints no floats without _printf_float (%f %e %g of the sensors printed nothing); the full
newlib printf costs 25 KB of the head. This one has what ESPurna writes: flags "-+ #0", width and precision (also *),
sizes hh h l ll z j t L, conversions d i u o x X c s p % f F e E g G. No %n, no positional arguments.

The format and the %s texts are read by aligned words: they are often in flash (PSTR, the .rodata of the modules),
where the ESP8266 has no byte loads (the SDK's LoadStoreError handler would serve every byte, slowly).

*/

static inline char flash_char(const char* p) {
    const uintptr_t address = (uintptr_t)p;
    const uint32_t word = *(const uint32_t*)(address & ~(uintptr_t)3);
    return (char)(word >> ((address & 3) * 8));
}

typedef struct {
    char* out;
    size_t size;
    size_t length;
} Sink;

static void sink_put(Sink* sink, char c) {
    if (sink->length + 1 < sink->size) {
        sink->out[sink->length] = c;
    }
    ++sink->length;
}

static void sink_repeat(Sink* sink, char c, int count) {
    while (count-- > 0) {
        sink_put(sink, c);
    }
}

enum {
    FlagLeft = 1,
    FlagPlus = 2,
    FlagSpace = 4,
    FlagAlt = 8,
    FlagZero = 16,
    FlagUpper = 32,
};

// a number already as digits (reversed in `digits`), with its sign / prefix, width, precision and flags
static void put_number(Sink* sink, const char* digits, int count, const char* prefix, int width, int precision,
                       int flags) {
    int prefix_length = 0;
    while (prefix[prefix_length]) {
        ++prefix_length;
    }

    const int zeros = (precision > count) ? (precision - count) : 0;
    int pad = width - prefix_length - zeros - count;

    if (!(flags & (FlagLeft | FlagZero))) {
        sink_repeat(sink, ' ', pad);
        pad = 0;
    }

    for (int index = 0; index < prefix_length; ++index) {
        sink_put(sink, prefix[index]);
    }

    if (flags & FlagZero) {
        sink_repeat(sink, '0', pad);
        pad = 0;
    }

    sink_repeat(sink, '0', zeros);
    while (count--) {
        sink_put(sink, digits[count]);
    }

    sink_repeat(sink, ' ', pad);
}

static void put_integer(Sink* sink, unsigned long long value, int negative, int base, int width, int precision,
                        int flags) {
    char digits[24];
    int count = 0;

    const char* alphabet = (flags & FlagUpper) ? "0123456789ABCDEF" : "0123456789abcdef";
    while (value) {
        digits[count++] = alphabet[value % (unsigned)base];
        value /= (unsigned)base;
    }

    // C: precision 0 and the value 0 print no digits; '#' with octal keeps one zero
    if (precision < 0) {
        precision = 1;
    } else {
        flags &= ~FlagZero;
    }

    char prefix[3] = {0, 0, 0};
    if (negative) {
        prefix[0] = '-';
    } else if (flags & FlagPlus) {
        prefix[0] = '+';
    } else if (flags & FlagSpace) {
        prefix[0] = ' ';
    } else if ((flags & FlagAlt) && count) {
        if (base == 16) {
            prefix[0] = '0';
            prefix[1] = (flags & FlagUpper) ? 'X' : 'x';
        } else if ((base == 8) && (precision <= count)) {
            precision = count + 1;
        }
    }

    put_number(sink, digits, count, prefix, width, precision, flags);
}

// the rounding error of a * b (Dekker): a * b == product + error exactly, double arithmetic only
static double product_error(double a, double b, double product) {
    const double split = 134217729.0;  // 2^27 + 1
    double c = split * a;
    const double a_high = c - (c - a);
    const double a_low = a - a_high;
    c = split * b;
    const double b_high = c - (c - b);
    const double b_low = b - b_high;
    return ((a_high * b_high - product) + a_high * b_low + a_low * b_high) + a_low * b_low;
}

// the decimal digits of a double >= 0 with `precision` places after the point (the point not written): value * 10^p
// as a 64-bit integer, an exact half rounded to even as the C libraries do; a value beyond the 64 bits keeps its 19
// leading digits and zeros behind them (ESPurna prints sensor values, not 1e300). Returns the count of the integer part.
static int float_digits(double value, int precision, char* digits, int size) {
    double scale = 1.0;
    for (int index = 0; index < precision; ++index) {
        scale *= 10.0;
    }

    // the places that do not fit in 64 bits are zeros on the right
    int zeros = 0;
    double scaled = value * scale;
    while (scaled >= 1.8e19) {
        scaled /= 10.0;
        ++zeros;
    }

    unsigned long long whole = (unsigned long long)scaled;
    const double rest = scaled - (double)whole;
    if (rest > 0.5) {
        ++whole;
    } else if (rest == 0.5) {
        // a half after the multiplication: the exact product decides (0.05 is 0.05000000000000000277), an exact half
        // goes to even
        const double error = zeros ? 0.0 : product_error(value, scale, value * scale);
        if ((error > 0) || ((error == 0) && (whole & 1))) {
            ++whole;
        }
    }

    // reversed: the zeros, then the digits, at least precision + 1 of them (0.05 -> "005")
    char reversed[40];
    int count = 0;
    while ((zeros > 0) && (count < (int)sizeof(reversed))) {
        reversed[count++] = '0';
        --zeros;
    }
    do {
        reversed[count++] = (char)('0' + (int)(whole % 10));
        whole /= 10;
    } while (whole && (count < (int)sizeof(reversed)));
    while ((count < precision + 1) && (count < (int)sizeof(reversed))) {
        reversed[count++] = '0';
    }

    if (count > size) {
        count = size;
    }

    for (int index = 0; index < count; ++index) {
        digits[index] = reversed[count - 1 - index];
    }

    return count - precision;
}

static void put_text(Sink* sink, const char* text, int length, int width, int flags) {
    const int pad = width - length;
    if (!(flags & FlagLeft)) {
        sink_repeat(sink, ' ', pad);
    }

    for (int index = 0; index < length; ++index) {
        sink_put(sink, text[index]);
    }

    if (flags & FlagLeft) {
        sink_repeat(sink, ' ', pad);
    }
}

static void put_float(Sink* sink, double value, char conversion, int width, int precision, int flags) {
    const int upper = (conversion == 'F') || (conversion == 'E') || (conversion == 'G');
    const char lower = (char)(conversion | 0x20);

    int negative = 0;
    if (value < 0 || ((value == 0) && (1.0 / value < 0))) {
        negative = 1;
        value = -value;
    }

    const char sign = negative ? '-' : (flags & FlagPlus) ? '+' : (flags & FlagSpace) ? ' ' : 0;

    if (value != value || value > 1.7976931348623157e308) {
        char text[5];
        int length = 0;
        if (sign) {
            text[length++] = sign;
        }
        const char* word = (value != value) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        for (int index = 0; index < 3; ++index) {
            text[length++] = word[index];
        }
        put_text(sink, text, length, width, flags & FlagLeft);
        return;
    }

    if (precision < 0) {
        precision = 6;
    }

    // %e / %g: the decimal exponent of the value
    int exponent = 0;
    if ((lower == 'e') || (lower == 'g')) {
        if (value != 0) {
            double scaled = value;
            while (scaled >= 10.0) {
                scaled /= 10.0;
                ++exponent;
            }
            while (scaled < 1.0) {
                scaled *= 10.0;
                --exponent;
            }
        }
    }

    int style = lower;
    int strip = 0;
    if (lower == 'g') {
        // C: P = the precision (0 -> 1); X = the exponent of %e; P > X >= -4: %f with P-1-X, else %e with P-1
        if (precision == 0) {
            precision = 1;
        }

        // the exponent after rounding to P digits (9.9999 -> 10.000)
        if (value != 0) {
            double rounded = value;
            for (int index = exponent; index > 0; --index) {
                rounded /= 10.0;
            }
            for (int index = exponent; index < 0; ++index) {
                rounded *= 10.0;
            }
            double half = 0.5;
            for (int index = 1; index < precision; ++index) {
                half /= 10.0;
            }
            if (rounded + half >= 10.0) {
                ++exponent;
            }
        }

        if ((precision > exponent) && (exponent >= -4)) {
            style = 'f';
            precision = precision - 1 - exponent;
        } else {
            style = 'e';
            precision = precision - 1;
        }

        strip = !(flags & FlagAlt);
    }

    double mantissa = value;
    if (style == 'e') {
        for (int index = exponent; index > 0; --index) {
            mantissa /= 10.0;
        }
        for (int index = exponent; index < 0; ++index) {
            mantissa *= 10.0;
        }
    }

    if (precision > 18) {
        precision = 18;
    }

    char digits[40];
    int integer = float_digits(mantissa, precision, digits, (int)sizeof(digits));

    // 9.99e5 rounded to 10.0e5
    if ((style == 'e') && (integer > 1)) {
        ++exponent;
        mantissa /= 10.0;
        integer = float_digits(mantissa, precision, digits, (int)sizeof(digits));
    }

    int length = integer + precision;
    if (strip) {
        while ((length > integer) && (digits[length - 1] == '0')) {
            --length;
        }
    }

    // the text: digits, '.', fraction, exponent
    char text[56];
    int size = 0;
    for (int index = 0; index < integer; ++index) {
        text[size++] = digits[index];
    }

    if ((length > integer) || (flags & FlagAlt)) {
        text[size++] = '.';
    }

    for (int index = integer; index < length; ++index) {
        text[size++] = digits[index];
    }

    if (style == 'e') {
        text[size++] = upper ? 'E' : 'e';
        text[size++] = (exponent < 0) ? '-' : '+';
        unsigned magnitude = (unsigned)((exponent < 0) ? -exponent : exponent);
        if (magnitude >= 100) {
            text[size++] = (char)('0' + magnitude / 100);
            magnitude %= 100;
        }
        text[size++] = (char)('0' + magnitude / 10);
        text[size++] = (char)('0' + magnitude % 10);
    }

    // sign and padding: the digits are not reversed here, put_number takes them reversed
    char reversed[56];
    for (int index = 0; index < size; ++index) {
        reversed[index] = text[size - 1 - index];
    }

    const char prefix[2] = {sign, 0};
    put_number(sink, reversed, size, prefix, width, -1, flags);
}

int vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
    Sink sink = {buffer, buffer ? size : 0, 0};

    for (;;) {
        char c = flash_char(format++);
        if (!c) {
            break;
        }

        if (c != '%') {
            sink_put(&sink, c);
            continue;
        }

        int flags = 0;
        for (;;) {
            c = flash_char(format++);
            if (c == '-') {
                flags |= FlagLeft;
            } else if (c == '+') {
                flags |= FlagPlus;
            } else if (c == ' ') {
                flags |= FlagSpace;
            } else if (c == '#') {
                flags |= FlagAlt;
            } else if (c == '0') {
                flags |= FlagZero;
            } else {
                break;
            }
        }

        int width = 0;
        if (c == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                flags |= FlagLeft;
                width = -width;
            }
            c = flash_char(format++);
        } else {
            while ((c >= '0') && (c <= '9')) {
                width = width * 10 + (c - '0');
                c = flash_char(format++);
            }
        }

        int precision = -1;
        if (c == '.') {
            precision = 0;
            c = flash_char(format++);
            if (c == '*') {
                precision = va_arg(args, int);
                c = flash_char(format++);
            } else {
                while ((c >= '0') && (c <= '9')) {
                    precision = precision * 10 + (c - '0');
                    c = flash_char(format++);
                }
            }
        }

        if (flags & FlagLeft) {
            flags &= ~FlagZero;
        }

        // sizes: 0 int, 1 char, 2 short, 3 long, 4 long long; size_t, intmax_t, ptrdiff_t: by their width
        int length = 0;
        if (c == 'h') {
            c = flash_char(format++);
            length = 2;
            if (c == 'h') {
                c = flash_char(format++);
                length = 1;
            }
        } else if (c == 'l') {
            c = flash_char(format++);
            length = 3;
            if (c == 'l') {
                c = flash_char(format++);
                length = 4;
            }
        } else if ((c == 'z') || (c == 't')) {
            c = flash_char(format++);
            length = (sizeof(size_t) > sizeof(long)) ? 4 : 3;
        } else if (c == 'j') {
            c = flash_char(format++);
            length = 4;
        } else if (c == 'L') {
            c = flash_char(format++);
        }

        switch (c) {
        case 'd':
        case 'i': {
            long long value;
            if (length == 4) {
                value = va_arg(args, long long);
            } else if (length == 3) {
                value = va_arg(args, long);
            } else {
                value = va_arg(args, int);
                if (length == 1) {
                    value = (signed char)value;
                } else if (length == 2) {
                    value = (short)value;
                }
            }

            const int negative = value < 0;
            const unsigned long long magnitude =
                negative ? (unsigned long long)0 - (unsigned long long)value : (unsigned long long)value;
            put_integer(&sink, magnitude, negative, 10, width, precision, flags & ~FlagAlt);
            break;
        }

        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long value;
            if (length == 4) {
                value = va_arg(args, unsigned long long);
            } else if (length == 3) {
                value = va_arg(args, unsigned long);
            } else {
                value = va_arg(args, unsigned int);
                if (length == 1) {
                    value = (unsigned char)value;
                } else if (length == 2) {
                    value = (unsigned short)value;
                }
            }

            const int base = (c == 'u') ? 10 : (c == 'o') ? 8 : 16;
            put_integer(&sink, value, 0, base, width, precision,
                        (flags & ~(FlagPlus | FlagSpace)) | ((c == 'X') ? FlagUpper : 0));
            break;
        }

        case 'p': {
            const uintptr_t value = (uintptr_t)va_arg(args, void*);
            put_integer(&sink, value, 0, 16, width, precision, FlagAlt | (flags & FlagLeft));
            break;
        }

        case 'c': {
            const char value = (char)va_arg(args, int);
            put_text(&sink, &value, 1, width, flags);
            break;
        }

        case 's': {
            const char* text = va_arg(args, const char*);
            if (!text) {
                text = "(null)";
            }

            int count = 0;
            while (((precision < 0) || (count < precision)) && flash_char(text + count)) {
                ++count;
            }

            const int pad = width - count;
            if (!(flags & FlagLeft)) {
                sink_repeat(&sink, ' ', pad);
            }

            for (int index = 0; index < count; ++index) {
                sink_put(&sink, flash_char(text + index));
            }

            if (flags & FlagLeft) {
                sink_repeat(&sink, ' ', pad);
            }
            break;
        }

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            put_float(&sink, va_arg(args, double), ((c | 0x20) == 'a') ? (char)(c + 6) : c, width, precision, flags);
            break;

        case '%':
            sink_put(&sink, '%');
            break;

        case '\0':
            --format;
            break;

        default:
            // an unknown conversion is printed as it is
            sink_put(&sink, '%');
            sink_put(&sink, c);
            break;
        }
    }

    if (sink.size) {
        sink.out[(sink.length < sink.size) ? sink.length : (sink.size - 1)] = '\0';
    }

    return (int)sink.length;
}

int snprintf(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int out = vsnprintf(buffer, size, format, args);
    va_end(args);
    return out;
}

int vsprintf(char* buffer, const char* format, va_list args) {
    return vsnprintf(buffer, (size_t)-1 >> 1, format, args);
}

int sprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int out = vsnprintf(buffer, (size_t)-1 >> 1, format, args);
    va_end(args);
    return out;
}

int vprintf(const char* format, va_list args) {
    return ets_vprintf(format, args);
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int out = ets_vprintf(format, args);
    va_end(args);
    return out;
}

int putchar(int c) {
    ets_putc(c);
    return c;
}

int puts(const char* text) {
    while (*text) {
        ets_putc(*text++);
    }

    ets_putc('\n');
    return 1;
}

void __assert_func(const char* file, int line, const char* function, const char* expression) {
    ets_printf("assert %s:%d %s: %s\n", file, line, function ? function : "", expression);
    abort();
}
