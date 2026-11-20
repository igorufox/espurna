/*

SYSTEM: the timers, the loop lock and the sleep of the SDK (system.h): the part of the FreeRTOS SDKs (ESP32, Beken) (the chip's folder, -I of its ini)

*/

#pragma once

#include "compat_chip.h"

namespace espurna {
namespace sleep {

using Microseconds = std::chrono::microseconds;

} // namespace sleep

namespace system {

// Common code is written for the esp8266 cooperative model, where network
// callbacks never overlap with loop(). Here AsyncTCP runs them in its own
// task (on ESP32 possibly on the other core). The loop task holds this lock for the
// whole iteration and releases it only while sleeping (see delay_unlocked()),
// async entry points take it before touching any shared state.
void lock();
void unlock();

// esp8266 espurna::core::chip::restart() only happens once the SDK gets to run again, the reboot here is immediate.
// Network modules report whether some of their output did not reach the peer yet (e.g. +OK of
// the RESET command, still queued or not acknowledged), restart() keeps waiting for it for a bit.
// Checks are called from the loop task, with the loop lock held.
using PendingOutput = bool (*)();
void registerPendingOutput(PendingOutput);

// loop() / setup() side, waits indefinitely
struct LoopGuard {
    LoopGuard() { lock(); }
    ~LoopGuard() { unlock(); }

    LoopGuard(const LoopGuard&) = delete;
    LoopGuard& operator=(const LoopGuard&) = delete;
};

// Chip part, arch/<chip>/system_<chip>.cpp. Only the SDK calls, everything around them is in system_rtos.cpp
namespace chip {
namespace sdk {

// last thing in systemSetup()
void setup();

// watchdog of the loop task, from the first loop() on (the rest of setup() is allowed to take its time).
// The chip tells the stall watcher its period, stall::period()
void enable_loop_wdt();

// loop task sleeps in delay_unlocked(), the watchdog that watches it has to be fed
void feed_loop_wdt();

// the actual reboot, once the network output is flushed and the settings are committed
[[noreturn]] void reboot();

// free heap and its largest free block (0 - the allocator does not track it)
void heap(uint32_t& free, uint32_t& block);

// mac(), light_sleep() and deep_sleep() are in the common system.h

} // namespace sdk
} // namespace chip

// The loop stall watcher (system_rtos.cpp): a task of its own, above every other one, keeps an eye on the loop task.
// - the loop watchdog is about to fire (fed() did not come for its period() minus a margin): the chip writes what it
//   keeps for after the reset (ESP32: the RTC record next to the core dump of the task WDT panic; Beken: the context of
//   every task in the flash record, the watchdog reset itself leaves nothing), cancelled when the feeding comes back
// - no loop() pass for SYSTEM_LOOP_STALL seconds while the watchdog is fed (a loop() waiting forever in something that
//   calls delay()): the same record, then the chip resets
namespace stall {

// what the watcher saw
struct Report {
    uint32_t callback;  // the loop callback the loop task stands in, espurnaLoopCurrent()
    uint32_t since;     // ms since the last feed (or the last loop() pass)
    uint32_t period;    // ms of the loop watchdog, 0 - not running
    uint32_t uptime;    // ms
};

// a loop() pass (every loop(); a long piece of work inside one pass may call it too, e.g. OTA writes)
void pass();

// the loop watchdog was fed, from any task
void fed();

// the loop watchdog was (re)started with this period in ms, 0 - stopped
void period(uint32_t ms);

// the module guard found the loop stalled (rtcmemStallSnapshot())
void snapshot();

// The watcher task's heartbeat, one more on every look it takes. The chip's tick hook counts its ticks while it stays
// the same: a task that never runs again (the lists of the scheduler broken) while the idle task still feeds the
// hardware watchdog (ESP8266, 11-18) - the hook resets
extern volatile uint32_t beat;

// what the tick hook writes for after that reset, from an ISR
Report lateReport();

// Chip part, arch/<chip>/
namespace chip {

// the watchdog is about to fire: what the chip keeps for after the reset (once, until cancel())
void record(const Report&);

// fed again in time, the record is not about a reset
void cancel();

// the loop stalled for good: record() and the reset
[[noreturn]] void reset(const Report&);

} // namespace chip
} // namespace stall
} // namespace system

namespace timer {

// Polled from loop() (ref. dispatch()), same as esp8266 os_timer callbacks never overlapping
// with loop(). Every timer is due at least DurationMin apart, loop() runs a lot more often.
// All state is guarded by the dispatcher mutex, so methods are also safe to call from other
// tasks (e.g. AsyncTCP handlers running without the loop lock), callbacks still run from loop().
struct SystemTimer {
    using TimeSource = time::CoreClock;
    using Duration = TimeSource::duration;

    static constexpr Duration DurationMin = Duration(5);

    SystemTimer();
    ~SystemTimer();

    SystemTimer(const SystemTimer&) = delete;
    SystemTimer& operator=(const SystemTimer&) = delete;

    // timer is bound to its id, an armed timer is not transferred (both are stopped)
    SystemTimer(SystemTimer&&);
    SystemTimer& operator=(SystemTimer&&);

    bool armed() const;
    explicit operator bool() const { return armed(); }

    void once(Duration duration, Callback callback);
    void repeat(Duration duration, Callback callback);
    void schedule_once(Duration, Callback);
    void stop();

    // runs every timer that is due, from loop()
    static void dispatch();

private:
    static constexpr Duration DurationMax = Duration(6870947);

    void start(Duration, Callback, bool repeat);

    // everything below is only touched with the dispatcher mutex held
    Callback _callback;
    uint32_t _id { 0 };
    bool _repeat { false };

    // micros64() based, us
    int64_t _period { 0 };
    int64_t _due { 0 };

    // bumped on every start() / stop() / one-shot dispatch
    uint32_t _generation { 0 };

    bool _armed { false };
};

} // namespace timer
} // namespace espurna
