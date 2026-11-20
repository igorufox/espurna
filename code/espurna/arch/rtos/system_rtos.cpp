/*

SYSTEM MODULE, part of the FreeRTOS chips (ESP32, BK72xx with LibreTiny)

Chip facts are in arch/<chip>/system_<chip>.cpp, common part of every chip is in system.cpp

*/

#include "espurna.h"

#include "espurna_tasks.h"
#include "rtcmem.h"
#include "storage_eeprom.h"

#include <algorithm>
#include <cstdlib>
#include <new>
#include <vector>

#include <lwip/tcpip.h>

// -----------------------------------------------------------------------------
// TASK PRIMITIVES (ref. espurna_tasks.h)
// -----------------------------------------------------------------------------

namespace espurna {
namespace system {

// criticalEnter() / criticalExit() are the chip's (arch/<chip>/system_<chip>.cpp)

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

// -----------------------------------------------------------------------------
// LOOP STALL WATCHER (ref. system_chip.h)
// -----------------------------------------------------------------------------

namespace stall {
namespace {

#ifndef SYSTEM_LOOP_STALL
#define SYSTEM_LOOP_STALL 60    // seconds without a loop() pass before the reset, 0 = off
#endif

constexpr uint32_t Seconds { SYSTEM_LOOP_STALL };

// periods below this are restart() and its short reset, not a loop that should feed
constexpr uint32_t PeriodMin { 3000 };
// written that much before the watchdog fires (a sector erase and a few writes take well below it)
constexpr uint32_t Margin { 1500 };
constexpr uint32_t Interval { 250 };
// bytes, the chip's record() and reset() run on it
constexpr uint32_t StackBytes { 2048 };

Shared<uint32_t> passes { 0 };
volatile uint32_t watchdog_period { 0 };
volatile TickType_t fed_at { 0 };

// a record is there, cancel() it once the feeding comes back
volatile bool recorded { false };

// the watcher's last look (lateReport())
volatile TickType_t watcher_at { 0 };

// The task of lwIP must take a call now and then: a board on the network that answers nothing (the tcpip task lost in
// broken scheduler lists, 11-18) while the loop still runs. A call every TcpipEvery, not taken for TcpipLate - reset
constexpr uint32_t TcpipEvery { 10000 };
constexpr uint32_t TcpipLate { 60000 };
volatile TickType_t tcpip_taken_at { 0 };
volatile bool tcpip_pending { false };
TickType_t tcpip_posted_at { 0 };

void tcpip_taken(void*) {
    tcpip_taken_at = xTaskGetTickCount();
    tcpip_pending = false;
}

// a marker in the report's callback field: the tcpip task, not a loop callback
constexpr uint32_t TcpipStalled { 0x7C9D0001 };

uint32_t ms(TickType_t ticks) {
    return static_cast<uint32_t>(ticks) * portTICK_PERIOD_MS;
}

Report report(uint32_t since) {
    return Report{
        .callback = static_cast<uint32_t>(espurnaLoopCurrent()),
        .since = since,
        .period = watchdog_period,
        .uptime = static_cast<uint32_t>(espurna::time::ms()),
    };
}

void task(void*) {
    auto seen = passes.load();
    auto passed_at = xTaskGetTickCount();

    tcpip_taken_at = passed_at;
    tcpip_posted_at = passed_at;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(Interval));

        const auto now = xTaskGetTickCount();
        watcher_at = now;
        ++beat;

        // the tcpip task: a call now and then, it must be taken
        if (!tcpip_pending && (ms(now - tcpip_posted_at) >= TcpipEvery)) {
            tcpip_posted_at = now;
            if (tcpip_try_callback(tcpip_taken, nullptr) == ERR_OK) {
                tcpip_pending = true;
            }
        }

        if (tcpip_pending && (ms(now - tcpip_taken_at) >= TcpipLate)) {
            auto out = report(ms(now - tcpip_taken_at));
            out.callback = TcpipStalled;
            chip::reset(out);
        }

        const auto current = passes.load();
        if (current != seen) {
            seen = current;
            passed_at = now;
        }

        const auto since_pass = ms(now - passed_at);
        if (Seconds && (since_pass >= (Seconds * 1000))) {
            chip::reset(report(since_pass));
        }

        const auto limit = watchdog_period;
        const auto since_fed = ms(now - fed_at);
        if ((limit >= PeriodMin) && ((since_fed + Margin) >= limit)) {
            if (!recorded) {
                recorded = true;
                chip::record(report(since_fed));
            }
        } else if (recorded) {
            recorded = false;
            chip::cancel();
        }
    }
}

void start() {
    fed_at = xTaskGetTickCount();
    watcher_at = fed_at;
    xTaskCreate(task, "stall", StackBytes / sizeof(StackType_t), nullptr, configMAX_PRIORITIES - 1, nullptr);
}

} // namespace

void pass() {
    passes.fetch_add(1);
    fed();
}

void fed() {
    fed_at = xTaskGetTickCount();
}

volatile uint32_t beat { 0 };

Report lateReport() {
    return Report{
        .callback = static_cast<uint32_t>(espurnaLoopCurrent()),
        .since = ms(xTaskGetTickCountFromISR() - watcher_at),
        .period = watchdog_period,
        .uptime = static_cast<uint32_t>(ms(xTaskGetTickCountFromISR())),
    };
}

void period(uint32_t value) {
    fed_at = xTaskGetTickCount();
    watchdog_period = value;
}

void snapshot() {
    recorded = true;
    chip::record(report(ms(xTaskGetTickCount() - fed_at)));
}

} // namespace stall

namespace chip {

void setup() {
    // the chip reads the record of the previous boot first
    chip::sdk::setup();
    stall::start();

    // Same as esp8266 soft WDT, reset when loop() gets stuck
    espurnaRegisterOnce([]() {
        chip::sdk::enable_loop_wdt();
    });
}

void loop() {
    stall::pass();
    timer::SystemTimer::dispatch();
}

[[noreturn]] void restart() {
    // esp8266 espurna::core::chip::restart() only takes effect once the SDK gets to run again, so network output
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
    ::espurna::core::chip::delay(ms);
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
    stall::fed();

    // the same as the ESP8266: the timers that come due while the loop task waits in delay() run inside it, in the
    // loop task with the loop lock (the SDK runs os_timer callbacks there), not only between loop() passes
    constexpr uint32_t Slice { 5 };
    const auto start = espurna::time::ms();
    for (;;) {
        timers_in_delay = true;
        timer::SystemTimer::dispatch();
        timers_in_delay = false;

        const uint32_t passed = espurna::time::ms() - start;
        if (passed >= ms) {
            break;
        }

        sleep_unlocked(std::min<uint32_t>(Slice, ms - passed));
    }

    // espurna::time::sleep(0): give the other tasks the CPU, as before
    if (!ms) {
        sleep_unlocked(0);
    }
}

} // namespace system
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

// the module guard found the loop stalled (modules.cpp): the chip's stall record, ref. stall::snapshot()
void rtcmemStallSnapshot() {
    espurna::system::stall::snapshot();
}

// a largest free block that is not tracked: the whole free heap counts as usable instead of 100% fragmentation
HeapStats systemHeapStats() {
    uint32_t free { 0 };
    uint32_t block { 0 };
    espurna::system::chip::sdk::heap(free, block);
    if (!block) {
        block = free;
    }

    const uint8_t frag = free ? (100 - (block * 100 / free)) : 0;
    return {free, block, frag};
}
