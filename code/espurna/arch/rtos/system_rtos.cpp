/*

SYSTEM MODULE, part of the FreeRTOS chips (ESP32, BK72xx with LibreTiny)

Chip facts are in arch/<chip>/system_<chip>.cpp, common part of every chip is in system.cpp

*/

#include "espurna.h"

#include "espurna_tasks.h"
#include "rtcmem.h"
#include "storage_eeprom.h"

#if SAFEBOOT_SUPPORT
#include "safeboot_esp32.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <new>
#include <vector>

// -----------------------------------------------------------------------------
// TASK PRIMITIVES (ref. espurna_tasks.h)
// -----------------------------------------------------------------------------

namespace espurna {
namespace system {

// The chip's part: ESP32 has two cores, a spinlock keeps the other one out (and masks the interrupts of this one),
// the BK72xx has one core and masking its interrupts is enough. Both nest
#if defined(ESP32)
namespace {

portMUX_TYPE critical_spinlock = portMUX_INITIALIZER_UNLOCKED;

} // namespace

uint32_t criticalEnter() {
    portENTER_CRITICAL(&critical_spinlock);
    return 0;
}

void criticalExit(uint32_t) {
    portEXIT_CRITICAL(&critical_spinlock);
}
#else
uint32_t criticalEnter() {
    return static_cast<uint32_t>(port_disable_interrupts_flag());
}

void criticalExit(uint32_t state) {
    port_enable_interrupts_flag(static_cast<int>(state));
}
#endif

Mutex::~Mutex() {
    if (_handle) {
        vSemaphoreDelete(_handle);
    }
}

// made outside of the critical section (allocates), the first one installed wins
SemaphoreHandle_t Mutex::handle() {
    SemaphoreHandle_t out;
    {
        Critical critical;
        out = _handle;
    }

    if (!out) {
        auto* made = xSemaphoreCreateMutex();
        {
            Critical critical;
            if (!_handle) {
                _handle = made;
                made = nullptr;
            }
            out = _handle;
        }

        if (made) {
            vSemaphoreDelete(made);
        }
    }

    return out;
}

// Owner and depth go together, both only ever set by the holder and read in the critical section. The depth tells
// a held mutex from a free one, the owner is no help there before the scheduler runs (no task, null handle)
bool Mutex::owned() const {
    const auto self = xTaskGetCurrentTaskHandle();

    Critical critical;
    return (_depth > 0) && (_owner == self);
}

void Mutex::held(size_t depth) {
    const auto self = depth ? xTaskGetCurrentTaskHandle() : nullptr;

    Critical critical;
    _owner = self;
    _depth = depth;
}

bool Mutex::take(TickType_t ticks) {
    if (owned()) {
        Critical critical;
        ++_depth;
        return true;
    }

    if (xSemaphoreTake(handle(), ticks) != pdTRUE) {
        return false;
    }

    held(1);
    return true;
}

void Mutex::unlock() {
    if (!owned()) {
        return;
    }

    size_t depth;
    {
        Critical critical;
        depth = --_depth;
    }

    if (!depth) {
        held(0);
        xSemaphoreGive(handle());
    }
}

size_t Mutex::release() {
    if (!owned()) {
        return 0;
    }

    size_t out;
    {
        Critical critical;
        out = _depth;
    }

    held(0);
    xSemaphoreGive(handle());

    return out;
}

void Mutex::reacquire(size_t depth) {
    if (!depth) {
        return;
    }

    xSemaphoreTake(handle(), portMAX_DELAY);
    held(depth);
}

} // namespace system
} // namespace espurna

// Global ESP object for the macro of compat_chip.h
EspCompat ESP_COMPAT;

// Same as esp8266, last 3 bytes of the MAC the chip id is made of (ref. systemChipId()), e.g. the SSDP serial number
uint32_t EspCompat::getChipId() {
    uint8_t mac[6] {};
    espurna::system::chip::mac(mac);
    return (uint32_t{mac[3]} << 16) | (uint32_t{mac[4]} << 8) | uint32_t{mac[5]};
}

// -----------------------------------------------------------------------------
// TIMERS (ref. system.h SystemTimer)
// -----------------------------------------------------------------------------

namespace espurna {
namespace timer {
namespace {

struct Entry {
    uint32_t id;
    SystemTimer* timer;
};

struct Dispatcher {
    system::Mutex mutex;
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
    system::MutexLock lock(d.mutex);

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
        system::MutexLock lock(d.mutex);

        const auto id = _id;
        d.live.erase(
            std::remove_if(d.live.begin(), d.live.end(),
                [&](const Entry& entry) {
                    return entry.id == id;
                }),
            d.live.end());

        _armed = false;
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
        system::MutexLock lock(d.mutex);

        const auto current = now();
        for (const auto& entry : d.live) {
            auto* timer = entry.timer;
            if (!timer->_armed || (current < timer->_due)) {
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
            system::MutexLock lock(d.mutex);

            auto* timer = find(d, entry.id);
            if (!timer || (timer->_generation != entry.generation) || !timer->_armed) {
                continue;
            }

            if (timer->_repeat) {
                callback = timer->_callback;
            } else {
                // one-shot is no longer armed by the time its callback runs, which also
                // allows callback to re-arm the timer (e.g. HASS discovery re-schedules
                // itself from send(), one entity per tick)
                callback.swap(timer->_callback);
                timer->_armed = false;
                ++timer->_generation;
            }
        }

        if (!callback.isEmpty()) {
            callback();
        }
    }
}

bool SystemTimer::armed() const {
    auto& d = dispatcher();
    system::MutexLock lock(d.mutex);
    return _armed;
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
        system::MutexLock lock(d.mutex);

        _armed = false;
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

        _armed = true;
    }
}

void SystemTimer::stop() {
    Callback previous;

    {
        auto& d = dispatcher();
        system::MutexLock lock(d.mutex);

        _armed = false;
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

// No VCC measurement, ADC_MODE_VALUE is forced to ADC_TOUT (ref. compat_chip.h). Callers
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
    espurna::system::chip::restart();
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

namespace chip {

void setup() {
    chip::sdk::setup();

    // Same as esp8266 soft WDT, reset when loop() gets stuck
    espurnaRegisterOnce([]() {
        chip::sdk::enable_loop_wdt();
    });
}

void loop() {
    chip::sdk::loop();
    timer::SystemTimer::dispatch();
}

[[noreturn]] void restart() {
    // esp8266 ESP.restart() only takes effect once the SDK gets to run again, so network output
    // queued right before it (e.g. +OK of the RESET command) still has a chance to be sent.
    // The reboot here is immediate, give the network tasks a moment to flush when called from loop().
    // Output may also be stuck waiting for a TCP retransmission (reply sent right after the WS
    // connection was opened), keep waiting until the peers acknowledged everything, but not forever
    if (loopTask()) {
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

    // a restart ESPurna asked for: a new image runs well enough, the bootloader keeps it (ESP32 rollback)
#if SAFEBOOT_SUPPORT
    safebootConfirm();
#endif

    chip::sdk::reboot();
}

} // namespace chip
} // namespace system
} // namespace espurna

// -----------------------------------------------------------------------------
// LOOP / ASYNCTCP SERIALIZATION (ref. system.h)
// -----------------------------------------------------------------------------

namespace espurna {
namespace system {
namespace {
namespace lock_impl {

Mutex mutex;

} // namespace lock_impl
} // namespace

void lock() {
    lock_impl::mutex.lock();
}

void unlock() {
    lock_impl::mutex.unlock();
}

namespace {

// a timer callback that calls delay() itself does not run the timers again (the ESP8266 SDK does not either)
bool timers_in_delay { false };

// sleep without the loop lock, the network tasks run meanwhile
void sleep_unlocked(uint32_t ms) {
    const auto depth = lock_impl::mutex.release();
    ::delay(ms);
    lock_impl::mutex.reacquire(depth);
}

} // namespace

void delay_unlocked(uint32_t ms) {
    // Same as esp8266 delay(), sleeping in the loop task keeps the watchdog happy
    // (the loop task itself only feeds it between loop() calls)
    if (!loopTask() || timers_in_delay) {
        sleep_unlocked(ms);
        return;
    }

    chip::sdk::feed_loop_wdt();

    // the same as the ESP8266: the timers that come due while the loop task waits in delay() run inside it, in the
    // loop task with the loop lock (the SDK runs os_timer callbacks there), not only between loop() passes
    constexpr uint32_t Slice { 5 };
    const auto start = millis();
    for (;;) {
        timers_in_delay = true;
        timer::SystemTimer::dispatch();
        timers_in_delay = false;

        const uint32_t passed = millis() - start;
        if (passed >= ms) {
            break;
        }

        sleep_unlocked(std::min<uint32_t>(Slice, ms - passed));
    }

    // delay(0): give the other tasks the CPU, as before
    if (!ms) {
        sleep_unlocked(0);
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

// -----------------------------------------------------------------------------
// NOTHROW OPERATOR NEW
// -----------------------------------------------------------------------------

// new (std::nothrow) of libstdc++ (GCC 9 and later) calls the throwing operator new inside a try. With the heap out
// the throw finds no memory for the exception either (__cxa_allocate_exception, no emergency pool), so the board ends
// in std::terminate -> abort instead of getting a nullptr: NSPanel 10.0.4.212 2026-11-16 (W83, the core dump: the mdns
// module's new[](nothrow) <- loop()); the BK72xx libstdc++_nano new calls abort(), which spins until the watchdog
// (10.0.5.139 2026-11-14, W1). Here nothrow means nothrow: malloc, nullptr when it fails. The head defines them and
// exports them (modules_exports.txt), the modules link against the head's (scripts/module_link.py), never a copy of
// libstdc++. ESP8266: its core (abi.cpp, no exceptions) has the same malloc ones already, a second definition would
// clash with them. The aligned variants (std::align_val_t) are not used by the head or any module
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return malloc(size ? size : 1);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return malloc(size ? size : 1);
}

