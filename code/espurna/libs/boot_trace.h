/*

THE BOOT TRACE OF THE ESP32 HEAD: the last step of its boot ESPurna reached, for the safeboot page after the
bootloader rolled a new image back (no serial log on most boards; 10.0.5.66 11-14: a watchdog reset, no core dump)

16 bytes of the RTC slow memory, at the same address in ESPurna and in code/safeboot-app: below what IDF reserves at
the end (_rtc_slow_reserved_start 0x50001FE8), far above the RTC data of both images. Kept through every reset but the
power-on one. ESPurna writes the step, safeboot reads it once and clears it

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espurna {
namespace boot_trace {

constexpr uintptr_t Address { 0x50001F00 };
constexpr uint32_t Magic { 0x45435254 }; // TRCE
constexpr size_t NameSize { 12 };

struct Trace {
    uint32_t magic;
    char name[NameSize];
};

inline volatile Trace* trace() {
    return reinterpret_cast<volatile Trace*>(Address);
}

inline void write(const char* name) {
    auto* out = trace();
    size_t index = 0;
    for (; (index < (NameSize - 1)) && name[index]; ++index) {
        out->name[index] = name[index];
    }
    for (; index < NameSize; ++index) {
        out->name[index] = '\0';
    }
    out->magic = Magic;
}

// the step, empty when nothing was written since the last read (or since power on)
inline void take(char (&out)[NameSize]) {
    auto* in = trace();
    out[0] = '\0';
    if (in->magic == Magic) {
        for (size_t index = 0; index < NameSize; ++index) {
            out[index] = in->name[index];
        }
        out[NameSize - 1] = '\0';
    }
    in->magic = 0;
}

} // namespace boot_trace
} // namespace espurna
