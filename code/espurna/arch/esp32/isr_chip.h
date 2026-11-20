/*

GPIO INTERRUPTS, ESP32 part (ref. espurna_isr.h)

noInterrupts() only masks the current core, the handler may run on the other one.
Shared state is guarded with a spinlock (critical section), which works across both cores.

*/

#pragma once

#include "../rtos/chip_sdk.h"

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <soc/gpio_reg.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace isr {

constexpr size_t Pins { 40 };

inline bool attachable(uint8_t pin) {
    return GPIO_IS_VALID_GPIO(pin);
}

// `mode`: the edge numbers of chip_sdk.h (CHANGE / FALLING / RISING of the common code have the same values)
inline void attach(uint8_t pin, void (*handler)(void*), void* arg, int mode) {
    ::espurna::core::chip::pin_attach(pin, handler, arg, static_cast<uint8_t>(mode));
}

// Direct register read, the handler must not call anything in flash
[[gnu::always_inline]]
inline bool read(uint8_t pin) {
    return (pin < 32)
        ? ((REG_READ(GPIO_IN_REG) >> pin) & 1)
        : ((REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1);
}

struct Lock {
    struct Guard {
        [[gnu::always_inline]]
        explicit Guard(Lock& lock) :
            _lock(lock)
        {
            portENTER_CRITICAL(&_lock._mux);
        }

        [[gnu::always_inline]]
        ~Guard() {
            portEXIT_CRITICAL(&_lock._mux);
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Lock& _lock;
    };

    struct IsrGuard {
        [[gnu::always_inline]]
        explicit IsrGuard(Lock& lock) :
            _lock(lock)
        {
            portENTER_CRITICAL_ISR(&_lock._mux);
        }

        [[gnu::always_inline]]
        ~IsrGuard() {
            portEXIT_CRITICAL_ISR(&_lock._mux);
        }

        IsrGuard(const IsrGuard&) = delete;
        IsrGuard& operator=(const IsrGuard&) = delete;

    private:
        Lock& _lock;
    };

    // in case the handler is still running on the other core, wait for it to finish
    static void sync(Lock& lock) {
        Guard guard(lock);
    }

    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace isr
} // namespace espurna
