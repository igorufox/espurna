/*

HOST: the chip calls of the tested sources (host/compat_chip.h), on the host's clock

*/

#include <espurna/types.h>
#include <espurna/system_time.h>

#include <chrono>
#include <thread>

namespace espurna {
namespace core {
namespace chip {

uint64_t micros() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void yield() {
    std::this_thread::yield();
}

} // namespace chip
} // namespace core

namespace system {

void delay_unlocked(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace system
} // namespace espurna

void ets_delay_us(uint32_t value) {
    std::this_thread::sleep_for(std::chrono::microseconds(value));
}
