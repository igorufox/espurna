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

// esp8266 ESP.restart() only happens once the SDK gets to run again, the reboot here is immediate.
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

// watchdog of the loop task, from the first loop() on (the rest of setup() is allowed to take its time)
void enable_loop_wdt();

// every loop(), before the timers and the deferred callbacks
void loop();

// loop task sleeps in delay_unlocked(), the watchdog that watches it has to be fed
void feed_loop_wdt();

// the actual reboot, once the network output is flushed and the settings are committed
[[noreturn]] void reboot();

// mac(), light_sleep() and deep_sleep() are in the common system.h

} // namespace sdk
} // namespace chip
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
