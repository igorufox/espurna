/*

SYSTEM MODULE, part of the FreeRTOS chips (ESP32, BK72xx with LibreTiny)

Chip facts are in arch/<chip>/system_<chip>.cpp, common part of every chip is in system.cpp

*/

#include "espurna.h"

#if defined(ESP32) || defined(LIBRETINY)

#include "rtcmem.h"
#include "storage_eeprom.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

// Arduino core main.cpp (ESP32), arch/beken/system_beken.cpp (LibreTiny)
extern TaskHandle_t loopTaskHandle;

// Global ESP object for the macro of compat_rtos.h
EspCompat ESP_COMPAT;

// -----------------------------------------------------------------------------
// TIMERS (ref. system_rtos.h SystemTimer)
// -----------------------------------------------------------------------------

namespace espurna {
namespace timer {
namespace {

struct Entry {
    uint32_t id;
    SystemTimer* timer;
};

struct Dispatcher {
    std::mutex mutex;
    std::vector<Entry> live;
    uint32_t next_id { 0 };
};

// function-local static, timers are constructed during static init
Dispatcher& dispatcher() {
    static Dispatcher out;
    return out;
}

SystemTimer* find(Dispatcher& d, uint32_t id) {
    const auto it = std::find_if(d.live.begin(), d.live.end(),
        [&](const Entry& entry) {
            return entry.id == id;
        });

    return (it != d.live.end()) ? (*it).timer : nullptr;
}

int64_t now() {
    return static_cast<int64_t>(micros64());
}

} // namespace

SystemTimer::SystemTimer() {
    auto& d = dispatcher();
    std::lock_guard<std::mutex> lock(d.mutex);

    ++d.next_id;
    if (!d.next_id) {
        ++d.next_id;
    }

    _id = d.next_id;
    d.live.push_back({_id, this});
}

SystemTimer::SystemTimer(SystemTimer&& other) :
    SystemTimer()
{
    other.stop();
}

SystemTimer& SystemTimer::operator=(SystemTimer&& other) {
    if (this != &other) {
        stop();
        other.stop();
    }

    return *this;
}

SystemTimer::~SystemTimer() {
    // captured objects are destroyed outside of the mutex, their destructors may use timers too
    Callback callback;

    {
        auto& d = dispatcher();
        std::lock_guard<std::mutex> lock(d.mutex);

        const auto id = _id;
        d.live.erase(
            std::remove_if(d.live.begin(), d.live.end(),
                [&](const Entry& entry) {
                    return entry.id == id;
                }),
            d.live.end());

        _armed.store(false, std::memory_order_release);
        ++_generation;

        callback.swap(_callback);
    }
}

void SystemTimer::dispatch() {
    auto& d = dispatcher();

    struct Ready {
        uint32_t id;
        uint32_t generation;
    };

    std::vector<Ready> ready;
    {
        std::lock_guard<std::mutex> lock(d.mutex);

        const auto current = now();
        for (const auto& entry : d.live) {
            auto* timer = entry.timer;
            if (!timer->armed() || (current < timer->_due)) {
                continue;
            }

            // repeating timer that missed some periods (loop() was busy) only runs once
            if (timer->_repeat && (timer->_period > 0)) {
                do {
                    timer->_due += timer->_period;
                } while (timer->_due <= current);
            }

            ready.push_back({entry.id, timer->_generation});
        }
    }

    for (const auto& entry : ready) {
        // Work on a copy, and never touch the timer object after the callback returns.
        // Callback is allowed to stop() / re-arm / destroy / move its own timer
        // (and to destroy the callable itself, e.g. by re-arming with a new one)
        Callback callback;

        {
            std::lock_guard<std::mutex> lock(d.mutex);

            auto* timer = find(d, entry.id);
            if (!timer || (timer->_generation != entry.generation) || !timer->armed()) {
                continue;
            }

            if (timer->_repeat) {
                callback = timer->_callback;
            } else {
                // one-shot is no longer armed by the time its callback runs, which also
                // allows callback to re-arm the timer (e.g. HASS discovery re-schedules
                // itself from send(), one entity per tick)
                callback.swap(timer->_callback);
                timer->_armed.store(false, std::memory_order_release);
                ++timer->_generation;
            }
        }

        if (!callback.isEmpty()) {
            callback();
        }
    }
}

void SystemTimer::once(Duration delay, Callback callback) {
    start(delay, std::move(callback), false);
}

void SystemTimer::repeat(Duration delay, Callback callback) {
    start(delay, std::move(callback), true);
}

void SystemTimer::start(Duration delay, Callback callback, bool repeat) {
    // previous callable is destroyed outside of the mutex, see ~SystemTimer()
    Callback previous;

    {
        auto& d = dispatcher();
        std::lock_guard<std::mutex> lock(d.mutex);

        _armed.store(false, std::memory_order_release);
        ++_generation;
        previous.swap(_callback);

        // same as esp8266, zero duration never fires
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(delay).count();
        if (us <= 0) {
            return;
        }

        _callback.swap(callback);
        _repeat = repeat;
        _period = us;
        _due = now() + us;

        _armed.store(true, std::memory_order_release);
    }
}

void SystemTimer::stop() {
    Callback previous;

    {
        auto& d = dispatcher();
        std::lock_guard<std::mutex> lock(d.mutex);

        _armed.store(false, std::memory_order_release);
        ++_generation;
        previous.swap(_callback);
    }
}

// callbacks already run from loop(), no need for another hop through espurnaRegisterOnce()
void SystemTimer::schedule_once(Duration delay, Callback callback) {
    once(delay, std::move(callback));
}

} // namespace timer
} // namespace espurna

// -----------------------------------------------------------------------------
// MEMORY
// -----------------------------------------------------------------------------

// No VCC measurement, ADC_MODE_VALUE is forced to ADC_TOUT (ref. compat_rtos.h). Callers
// (debug.cpp / influxdb.cpp / mqtt.cpp / ws.cpp) are gated by `ADC_MODE_VALUE == ADC_VCC`,
// the definition still has to exist for the linker to resolve EspCompat::getVcc().
uint16_t systemVcc() {
    return 0;
}

// -----------------------------------------------------------------------------
// SDK CONFIG
// -----------------------------------------------------------------------------

[[noreturn]] void forceEraseSDKConfig() {
    eraseSDKConfig();
    customResetReason(CustomResetReason::Terminal);
    espurna::system::arch::restart();
}

// -----------------------------------------------------------------------------
// COMMON SYSTEM MODULE HOOKS (ref. system.h)
// -----------------------------------------------------------------------------

namespace espurna {
namespace system {
namespace {
namespace pending_output {

// ws + telnet, only registered once from their setup()
constexpr size_t ChecksMax { 4 };
PendingOutput checks[ChecksMax] {};

bool any() {
    for (const auto& check : checks) {
        if (check && check()) {
            return true;
        }
    }

    return false;
}

} // namespace pending_output
} // namespace

void registerPendingOutput(PendingOutput check) {
    for (auto& slot : pending_output::checks) {
        if (!slot || (slot == check)) {
            slot = check;
            return;
        }
    }
}

namespace arch {

void setup() {
    chip::setup();

    // Same as esp8266 soft WDT, reset when loop() gets stuck
    espurnaRegisterOnce([]() {
        chip::enable_loop_wdt();
    });
}

void loop() {
    chip::loop();
    timer::SystemTimer::dispatch();
}

[[noreturn]] void restart() {
    // esp8266 ESP.restart() only takes effect once the SDK gets to run again, so network output
    // queued right before it (e.g. +OK of the RESET command) still has a chance to be sent.
    // The reboot here is immediate, give the network tasks a moment to flush when called from loop().
    // Output may also be stuck waiting for a TCP retransmission (reply sent right after the WS
    // connection was opened), keep waiting until the peers acknowledged everything, but not forever
    if (xTaskGetCurrentTaskHandle() == loopTaskHandle) {
        constexpr uint32_t FlushMin { 250 };
        constexpr uint32_t FlushMax { 3000 };
        constexpr uint32_t FlushInterval { 50 };

        delay_unlocked(FlushMin);
        for (uint32_t waited = FlushMin; (waited < FlushMax) && pending_output::any(); waited += FlushInterval) {
            delay_unlocked(FlushInterval);
        }
    }

    // settings are committed by the loop, make sure the last change is not lost
    eepromForceCommit();

    chip::reboot();
}

} // namespace arch
} // namespace system
} // namespace espurna

// -----------------------------------------------------------------------------
// LOOP / ASYNCTCP SERIALIZATION (ref. system_rtos.h)
// -----------------------------------------------------------------------------

namespace espurna {
namespace system {
namespace {
namespace lock_impl {

constexpr auto AsyncTimeout = duration::Milliseconds{ 100 };

SemaphoreHandle_t mutex { nullptr };
std::atomic<TaskHandle_t> owner { nullptr };
size_t depth { 0 };

// First lock() happens from the loop task in setup(), before any network task is running
SemaphoreHandle_t handle() {
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
    }

    return mutex;
}

bool take(TickType_t ticks) {
    const auto self = xTaskGetCurrentTaskHandle();
    if (owner.load() == self) {
        ++depth;
        return true;
    }

    if (xSemaphoreTake(handle(), ticks) != pdTRUE) {
        return false;
    }

    owner.store(self);
    depth = 1;

    return true;
}

void give() {
    if (owner.load() != xTaskGetCurrentTaskHandle()) {
        return;
    }

    if (--depth == 0) {
        owner.store(nullptr);
        xSemaphoreGive(handle());
    }
}

} // namespace lock_impl
} // namespace

void lock() {
    lock_impl::take(portMAX_DELAY);
}

bool lock(duration::Milliseconds timeout) {
    return lock_impl::take(pdMS_TO_TICKS(timeout.count()));
}

void unlock() {
    lock_impl::give();
}

void delay_unlocked(uint32_t ms) {
    const auto self = xTaskGetCurrentTaskHandle();

    // Same as esp8266 delay(), sleeping in the loop task keeps the watchdog happy
    // (the loop task itself only feeds it between loop() calls)
    if (self == loopTaskHandle) {
        chip::feed_loop_wdt();
    }

    if (lock_impl::owner.load() != self) {
        ::delay(ms);
        return;
    }

    const auto depth = lock_impl::depth;
    lock_impl::depth = 0;
    lock_impl::owner.store(nullptr);
    xSemaphoreGive(lock_impl::handle());

    ::delay(ms);

    xSemaphoreTake(lock_impl::handle(), portMAX_DELAY);
    lock_impl::owner.store(self);
    lock_impl::depth = depth;
}

AsyncGuard::AsyncGuard() :
    _locked(lock(lock_impl::AsyncTimeout))
{
    // Safe from any task, debug output coming from outside of the loop task is queued
    // (ref. debug.cpp deferred::push()) and sent out by loop() later
    if (!_locked) {
        DEBUG_MSG_P(PSTR("[MAIN] Loop is busy, network handler could not take the lock\n"));
    }
}

AsyncGuard::~AsyncGuard() {
    if (_locked) {
        unlock();
    }
}

} // namespace system
} // namespace espurna

// -----------------------------------------------------------------------------
// SETTINGS HELPERS
// -----------------------------------------------------------------------------

namespace espurna {
namespace settings {
namespace internal {

template <>
StringSumHelper convert(const String& v) {
    return StringSumHelper(v);
}

} // namespace internal
} // namespace settings
} // namespace espurna

#endif
