/*

GPIO INTERRUPTS, ESP8266 on ESP8266_RTOS_SDK (ref. espurna_isr.h)

One core: masking the interrupts is enough (the FreeRTOS critical section of the port nests)

*/

#pragma once


#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace isr {

// GPIO0..15 (GPIO16 is the RTC pad, it has no interrupt)
constexpr size_t Pins { 16 };

inline bool attachable(uint8_t pin) {
    return pin < Pins;
}

inline void attach(uint8_t pin, void (*handler)(void*), void* arg, int mode) {
    ::espurna::core::chip::pin_attach(pin, handler, arg, static_cast<uint8_t>(mode));
}

// GPIO_IN register, the handler must not call anything in flash
[[gnu::always_inline]]
inline bool read(uint8_t pin) {
    return ((*reinterpret_cast<volatile uint32_t*>(0x60000318)) >> pin) & 1;
}

struct Lock {
    struct Guard {
        explicit Guard(Lock&) {
            portENTER_CRITICAL();
        }

        ~Guard() {
            portEXIT_CRITICAL();
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    // the handler runs with the interrupts off already
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
