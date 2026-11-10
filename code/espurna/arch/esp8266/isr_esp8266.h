/*

GPIO INTERRUPTS, esp8266 part (ref. espurna_isr.h)

The handler always runs on the same (only) core, masking interrupts is enough
and the handler itself does not need any extra protection.

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace isr {

constexpr size_t Pins { EXTERNAL_NUM_INTERRUPTS };

inline bool attachable(uint8_t pin) {
    return digitalPinToInterrupt(pin) != NOT_AN_INTERRUPT;
}

inline void attach(uint8_t pin, void (*handler)(void*), void* arg, int mode) {
    attachInterruptArg(pin, handler, arg, mode);
}

[[gnu::always_inline]]
inline bool read(uint8_t pin) {
    return GPIP(pin);
}

struct Lock {
    struct Guard {
        [[gnu::always_inline]]
        explicit Guard(Lock&) {
            noInterrupts();
        }

        [[gnu::always_inline]]
        ~Guard() {
            interrupts();
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    struct IsrGuard {
        [[gnu::always_inline]]
        explicit IsrGuard(Lock&) {
        }

        [[gnu::always_inline]]
        ~IsrGuard() {
        }
    };

    static void sync(Lock&) {
    }
};

} // namespace isr
} // namespace espurna
