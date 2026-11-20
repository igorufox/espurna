/*

ESPurna's own text types, without the layer of the Arduino names (owner 2026-11-18: the code of ESPurna itself without
lib/espurna-core, the smallest variant).

- reading: espurna::StringView (types.h), a pointer and a length
- keeping: espurna::string, a pointer and a length on the heap, terminated for C calls; only where a text has to be
  kept (settings values, names); it grows by the exact size of what is appended, nothing reserved ahead
- writing out: espurna::Output, one virtual write(pointer, length) and one printf over vsnprintf (instead of Print and
  Stream with their print(int) / println(float) overloads)

*/

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "types.h"

#include <type_traits>

namespace espurna {

// Where text goes: the terminal, the debug log, telnet, a web response, a buffer. One virtual write(); the rest are
// the calls the code makes, all on top of it (numbers through printf)
struct Output {
    virtual ~Output() = default;

    virtual void write(const char* data, size_t size) = 0;

    // what was written has left (a port with its own buffer)
    virtual void flush() {
    }

    void write(const uint8_t* data, size_t size) {
        write(reinterpret_cast<const char*>(data), size);
    }

    void write(StringView value) {
        write(value.data(), value.length());
    }

    void write(char value) {
        write(&value, 1);
    }

    // a text: StringView, a C string, a literal. print() and println() return the bytes written
    template <typename T>
    size_t print(const T& value) {
        const StringView view(value);
        write(view);
        return view.length();
    }

    size_t print(char value) {
        write(&value, 1);
        return 1;
    }

    size_t print(unsigned char value) {
        return printf("%u", static_cast<unsigned>(value));
    }

    size_t print(int value) {
        return printf("%d", value);
    }

    size_t print(unsigned value) {
        return printf("%u", value);
    }

    size_t print(long value) {
        return printf("%ld", value);
    }

    size_t print(unsigned long value) {
        return printf("%lu", value);
    }

    template <typename T>
    size_t println(const T& value) {
        return print(value) + print(static_cast<char>(10));
    }

    size_t println() {
        return print(static_cast<char>(10));
    }

    size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
    size_t vprintf(const char* format, va_list args);
};

} // namespace espurna
