/*

SYSTEM MODULE

Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>

Common part, shared by every chip, implemented by system.cpp. What follows the execution model of the SDK (timers,
the loop lock, sleep durations) has one part per model below: the esp8266 NONOS SDK runs network callbacks and timers
in SYS, between loop() calls (arch/esp8266/system_esp8266.cpp), the FreeRTOS SDKs of ESP32 and LibreTiny run them in
tasks of their own (arch/rtos/system_rtos.cpp). The chip itself (reboot, watchdog, reset reason, sleep, MAC) is
arch/<chip>/system_<chip>.cpp, ref. espurna::system::chip

*/

#pragma once

#include "system_time.h"
#include "settings.h"
#include "types.h"

#include <chrono>
#include <cstdint>
#include <memory>

#if defined(ESP8266)

#include <user_interface.h>

namespace espurna {
namespace sleep {

// Both LIGHT and DEEP sleep accept microseconds as input
// Effective limit is ~31bit - 1 in size
using Microseconds = std::chrono::duration<uint32_t, std::micro>;

} // namespace sleep

namespace system {

// Network callbacks run in SYS and never overlap with loop(), nothing to serialize.
// (ref. arch/esp32, where they run in a separate task)
struct LoopGuard {
    LoopGuard() {}
};

struct AsyncGuard {
    AsyncGuard() {}
    explicit AsyncGuard(duration::Milliseconds) {}

    bool locked() const {
        return true;
    }
};

} // namespace system

namespace timer {

struct SystemTimer {
    using TimeSource = time::CoreClock;
    using Duration = TimeSource::duration;

    static constexpr Duration DurationMin = Duration(5);

    SystemTimer();
    ~SystemTimer() {
        stop();
    }

    SystemTimer(const SystemTimer&) = delete;
    SystemTimer& operator=(const SystemTimer&) = delete;

    SystemTimer(SystemTimer&&) = default;
    SystemTimer& operator=(SystemTimer&&) = default;

    bool armed() const {
        return _armed != nullptr;
    }

    explicit operator bool() const {
        return armed();
    }

    void once(Duration duration, Callback callback) {
        start(duration, std::move(callback), false);
    }

    void repeat(Duration duration, Callback callback) {
        start(duration, std::move(callback), true);
    }

    void schedule_once(Duration, Callback);
    void stop();

private:
    // limit is per https://www.espressif.com/sites/default/files/documentation/2c-esp8266_non_os_sdk_api_reference_en.pdf
    // > 3.1.1 os_timer_arm
    // > with `system_timer_reinit()`, the timer value allowed ranges from 100 to 0x0x689D0.
    // > otherwise, the timer value allowed ranges from 5 to 0x68D7A3.
    // with current implementation we use division by 2 until we reach value less than this one
    static constexpr Duration DurationMax = Duration(6870947);

    void reset();
    void start(Duration, Callback, bool repeat);
    void callback();

    struct Tick {
        size_t total;
        size_t count;
    };

    Callback _callback;

    os_timer_t* _armed { nullptr };
    bool _repeat { false };

    std::unique_ptr<Tick> _tick;
    std::unique_ptr<os_timer_t> _timer;
};

} // namespace timer

namespace settings {
namespace internal {

String serialize(duration::ClockCycles);

} // namespace internal
} // namespace settings
} // namespace espurna

#elif defined(ESP32) || defined(LIBRETINY)

#include "arch/rtos/compat_rtos.h"

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
bool lock(duration::Milliseconds timeout);
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

// AsyncTCP side. Waiting is bounded: AsyncTCP task stalling for too long lets
// its event queue fill up, which blocks the tcpip task and whoever is waiting
// on it. On timeout, handler should hand the work over to loop() instead
// (ref. runInLoop() in espurna_tasks.h), or reject it (e.g. HTTP 503). Proceeding unlocked
// races with loop() and is only acceptable when nothing shared is touched.
struct AsyncGuard {
    AsyncGuard();

    // waits longer than the default, e.g. for every chunk of a firmware upload, which cannot be dropped or deferred
    explicit AsyncGuard(duration::Milliseconds timeout);
    ~AsyncGuard();

    AsyncGuard(const AsyncGuard&) = delete;
    AsyncGuard& operator=(const AsyncGuard&) = delete;

    bool locked() const {
        return _locked;
    }

private:
    bool _locked;
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

#endif

namespace espurna {
namespace sleep {

// light sleep timer limits, 0xFFFFFFF is 'until woken up' of the esp8266 NONOS API (wifi_fpm_do_sleep)
constexpr auto FpmSleepMin = Microseconds{ 1000 };
constexpr auto FpmSleepIndefinite = Microseconds{ 0xFFFFFFF };

} // namespace sleep
} // namespace espurna

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>

struct HeapStats {
    uint32_t available;
    uint32_t usable;
    uint8_t fragmentation;
};

enum class CustomResetReason : uint8_t {
    None,
    Button,    // button event action
    Factory,   // requested factory reset
    Hardware,  // driver event
    Mqtt,
    Ota,       // successful ota
    Rpc,       // rpc (api) calls
    Rule,      // rpn rule operator action
    Scheduler, // scheduled reset
    Terminal,  // terminal command action
    Web,       // webui action
    Stability, // stable counter action
    Network,   // the network stack stopped receiving while Wi-Fi stayed connected
    Modules,   // the module loader: /modules/off, the Apply, /modules/on, the panel's restart
};

namespace espurna {
namespace system {

struct RandomDevice {
    using result_type = uint32_t;

    static constexpr result_type min() {
        return std::numeric_limits<result_type>::min();
    }

    static constexpr result_type max() {
        return std::numeric_limits<result_type>::max();
    }

    uint32_t operator()() const;
};

// Implemented by arch/<chip>/system_<chip>.cpp, used by the common system module
namespace chip {

// first thing in systemSetup(), before anything else is initialized
void pre_setup();

// last thing in systemSetup()
void setup();

// every loop(), before the common part
void loop();

// reason of the current boot, as REASON_* value
uint32_t reset_reason();

// actual reboot, custom reason is already stored at this point
[[noreturn]] void restart();

} // namespace chip
} // namespace system

struct ReadyFlag {
    bool wait(duration::Milliseconds);
    void stop();

    bool stop_wait(duration::Milliseconds duration) {
        stop();
        return wait(duration);
    }

    bool ready() const {
        return _ready;
    }

    explicit operator bool() const {
        return ready();
    }

private:
    bool _ready { true };
    timer::SystemTimer _timer;
};

struct PolledReadyFlag {
    bool wait(duration::Milliseconds);
    void stop();

    bool stop_wait(duration::Milliseconds duration) {
        stop();
        return wait(duration);
    }

    bool ready();

    explicit operator bool() {
        return ready();
    }

private:
    bool _ready { true };
    time::SystemClock::time_point _until{};
};

template <typename T>
struct PolledFlag {
    bool wait(typename T::duration);

    void reset() {
        _last = T::now();
    }

protected:
    typename T::time_point _last { T::now() };
};

template <typename T>
bool PolledFlag<T>::wait(typename T::duration interval) {
    const auto now = T::now();
    if (now - _last > interval) {
        _last = now;
        return true;
    }

    return false;
}

namespace heartbeat {

using Mask = int32_t;
using Callback = bool(*)(Mask);

enum class Mode {
    None,
    Once,
    Repeat
};

enum class Report : Mask {
    Status = 1 << 1,
    Ssid = 1 << 2,
    Ip = 1 << 3,
    Mac = 1 << 4,
    Rssi = 1 << 5,
    Uptime = 1 << 6,
    Datetime = 1 << 7,
    Freeheap = 1 << 8,
    Vcc = 1 << 9,
    Relay = 1 << 10,
    Light = 1 << 11,
    Hostname = 1 << 12,
    App = 1 << 13,
    Version = 1 << 14,
    Board = 1 << 15,
    Loadavg = 1 << 16,
    Interval = 1 << 17,
    Description = 1 << 18,
    Range = 1 << 19,
    RemoteTemp = 1 << 20,
    Bssid = 1 << 21
};

constexpr Mask operator*(Report lhs, Mask rhs) {
    return static_cast<Mask>(lhs) * rhs;
}

constexpr Mask operator*(Mask lhs, Report rhs) {
    return lhs * static_cast<Mask>(rhs);
}

constexpr Mask operator|(Report lhs, Report rhs) {
    return static_cast<Mask>(lhs) | static_cast<Mask>(rhs);
}

constexpr Mask operator|(Report lhs, Mask rhs) {
    return static_cast<Mask>(lhs) | rhs;
}

constexpr Mask operator|(Mask lhs, Report rhs) {
    return lhs | static_cast<Mask>(rhs);
}

constexpr Mask operator&(Report lhs, Mask rhs) {
    return static_cast<Mask>(lhs) & rhs;
}

constexpr Mask operator&(Mask lhs, Report rhs) {
    return lhs & static_cast<Mask>(rhs);
}

constexpr Mask operator&(Report lhs, Report rhs) {
    return static_cast<Mask>(lhs) & static_cast<Mask>(rhs);
}

espurna::duration::Seconds currentInterval();
espurna::duration::Milliseconds currentIntervalMs();

Mask currentValue();
Mode currentMode();

} // namespace heartbeat

namespace sleep {

enum class Interrupt {
    Low,
    High,
};

namespace settings {

// wakeup source of instantLightSleep(), GPIO_NONE when not configured
uint8_t pin();
Interrupt interrupt();

} // namespace settings
} // namespace sleep

namespace system {

// Chip part of every chip, arch/<chip>/system_<chip>.cpp. Only the SDK calls, everything around them is in
// system.cpp (and for the FreeRTOS chips in arch/rtos/system_rtos.cpp, ref. its own chip part)
namespace chip {

// MAC the chip id is made of (6 bytes), false when it could not be read
bool mac(uint8_t* out);

// instantLightSleep() and instantDeepSleep(), either a timer or a pin wakes the chip up.
// Callbacks of systemBeforeSleep() / systemAfterSleep() are run by the chip, ref. run_before_sleep()
bool light_sleep(bool timer, sleep::Microseconds time, int pin, sleep::Interrupt interrupt);
bool deep_sleep(sleep::Microseconds time);

// First thing in setup(), before anything reads the settings. esp8266: the 1 MB head on a 4 MB board moves the
// settings of the old 4 MB layout (pool at the end of the chip) into its own pool, once
void early_setup();

} // namespace chip

void run_before_sleep();
void run_after_sleep();

} // namespace system

namespace settings {
namespace internal {

template <>
heartbeat::Mode convert(const String&);

String serialize(heartbeat::Mode);

template <>
sleep::Interrupt convert(const String&);

String serialize(sleep::Interrupt);

} // namespace internal
} // namespace settings
} // namespace espurna

uint32_t randomNumber(uint32_t minimum, uint32_t maximum);
uint32_t randomNumber();

unsigned long systemFreeStack();

HeapStats systemHeapStats();

size_t systemFreeHeap();
size_t systemInitialFreeHeap();

[[noreturn]] void forceEraseSDKConfig();

bool eraseSDKConfig();
void factoryReset();

uint32_t systemResetReason();
uint8_t systemStabilityCounter();
void systemStabilityCounter(uint8_t count);

void systemForceStable();
void systemForceUnstable();
bool systemCheck();

void customResetReason(CustomResetReason);
CustomResetReason customResetReason();
String customResetReasonToPayload(CustomResetReason);

void deferredReset(espurna::duration::Milliseconds, CustomResetReason);
void prepareReset(CustomResetReason);
bool pendingDeferredReset();

bool wakeupModemForcedSleep();
bool prepareModemForcedSleep();

using SleepCallback = void (*)();
void systemBeforeSleep(SleepCallback);
void systemAfterSleep(SleepCallback);

bool instantLightSleep();
bool instantLightSleep(espurna::sleep::Microseconds);
bool instantLightSleep(uint8_t pin, espurna::sleep::Interrupt);

bool instantDeepSleep(espurna::sleep::Microseconds);

unsigned long systemLoadAverage();

espurna::duration::Seconds systemHeartbeatInterval();
void systemScheduleHeartbeat();

void systemStopHeartbeat(espurna::heartbeat::Callback);
void systemHeartbeat(espurna::heartbeat::Callback, espurna::heartbeat::Mode, espurna::duration::Seconds interval);
void systemHeartbeat(espurna::heartbeat::Callback, espurna::heartbeat::Mode);
void systemHeartbeat(espurna::heartbeat::Callback);
bool systemHeartbeat();

espurna::duration::Seconds systemUptime();

espurna::StringView systemDevice();
String systemManufacturer();
String systemDeviceName();
espurna::StringView systemIdentifier();

espurna::StringView systemChipId();
espurna::StringView systemShortChipId();

espurna::StringView systemDefaultPassword();

String systemPassword();
bool systemPasswordEquals(espurna::StringView);

String systemHostname();
String systemDescription();

void systemSetup();
void systemSetupUnstable();
