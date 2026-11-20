/*

GPIO INTERRUPTS, BK72xx part (ref. espurna_isr.h)

Single core, but noInterrupts() is not implemented by the ARM968 port.

*/

#pragma once

#include <chip_sdk.h>

// port_disable_interrupts_flag()
#include "compat_sdk.h"

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace isr {

// the GPIO numbers of the BK7231N, P0 .. P28
constexpr size_t Pins { 29 };

// any pin of the table
inline bool attachable(uint8_t) {
    return true;
}

inline void attach(uint8_t pin, void (*handler)(void*), void* arg, int mode) {
    ::espurna::core::chip::pin_attach(pin, handler, arg, static_cast<uint8_t>(mode));
}

inline bool read(uint8_t pin) {
    return ::espurna::core::chip::pin_read(pin);
}

struct Lock {
    struct Guard {
        explicit Guard(Lock&) :
            _flag(port_disable_interrupts_flag())
        {
        }

        ~Guard() {
            port_enable_interrupts_flag(_flag);
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        decltype(port_disable_interrupts_flag()) _flag;
    };

    struct IsrGuard {
        explicit IsrGuard(Lock&) {
        }

        ~IsrGuard() {
        }
    };

    static void sync(Lock&) {
    }
};

} // namespace isr
} // namespace espurna
