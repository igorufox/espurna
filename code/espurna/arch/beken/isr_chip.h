/*

GPIO INTERRUPTS, LibreTiny part (ref. espurna_isr.h)

Single core, but noInterrupts() is not implemented by the ARM968 port.

*/

#pragma once

#include <Arduino.h>

// port_disable_interrupts_flag()
#include "compat_beken.h"

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace isr {

constexpr size_t Pins { PINS_GPIO_MAX + 1 };

// any pin of the table
inline bool attachable(uint8_t) {
    return true;
}

inline void attach(uint8_t pin, void (*handler)(void*), void* arg, int mode) {
    attachInterruptParam(pin, handler, static_cast<PinStatus>(mode), arg);
}

inline bool read(uint8_t pin) {
    return ::digitalRead(pin) == HIGH;
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
