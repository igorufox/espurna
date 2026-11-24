/*

HOST: GPIO interrupts of the chip (ref. espurna_isr.h) for the unit tests: no pins, nothing to mask

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace isr {

constexpr size_t Pins { 0 };

inline bool attachable(uint8_t) {
    return false;
}

inline void attach(uint8_t, void (*)(void*), void*, int) {
}

inline bool read(uint8_t) {
    return false;
}

struct Lock {
    struct Guard {
        explicit Guard(Lock&) {
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    struct IsrGuard {
        explicit IsrGuard(Lock&) {
        }
    };

    static void sync(Lock&) {
    }
};

} // namespace isr
} // namespace espurna
