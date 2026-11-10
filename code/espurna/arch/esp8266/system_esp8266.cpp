/*

SYSTEM MODULE FOR ESP8266

Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>

Chip specific part, common part is in system.cpp

*/

#include "../../espurna.h"

#include "../../rtcmem.h"
#include "../../crash.h"

#if MODULES_SUPPORT
#include "../../modules.h"
#endif

#include <cstdint>
#include <cstring>
#include <forward_list>
#include <vector>

extern "C" {
#include "user_interface.h"
extern struct rst_info resetInfo;
}

#include "libs/TypeChecks.h"

// -----------------------------------------------------------------------------

// This method is called by the SDK early on boot to know where to connect the ADC
// Notice that current Core versions automatically de-mangle the function name for historical reasons
// (meaning, it is already used as `_Z14__get_adc_modev` and there's no need for `extern "C"`)
int __get_adc_mode() {
    return (int) (ADC_MODE_VALUE);
}

// Exposed through libphy.a in the current NONOS, may be replaced with a direct call to `os_random()` / `esp_random()`
extern "C" unsigned long adc_rand_noise;

// -----------------------------------------------------------------------------

namespace espurna {
namespace settings {
namespace internal {

String serialize(espurna::duration::ClockCycles value) {
    return serialize(value.count());
}

} // namespace internal
} // namespace settings

// TODO: implement 'before' and 'after' callbacks executed outside of normal loop
// TODO: flush HW UART before light sleep?

namespace sleep {
namespace {

constexpr auto DeepSleepWakeupPin = uint8_t{ 16 };

// 0xFFFFFFF is a magic number per the NONOS API reference, 3.7.5 wifi_fpm_do_sleep:
// > If sleep_time_in_us is 0xFFFFFFF, the ESP8266 will sleep till be woke up as below:
// > • If wifi_fpm_set_sleep_type is set to be LIGHT_SLEEP_T, ESP8266 can wake up by GPIO.
// > • If wifi_fpm_set_sleep_type is set to be MODEM_SLEEP_T, ESP8266 can wake up by wifi_fpm_do_wakeup.
//
// In our case, both sleep modes are indefinite when OFF action is executed.
// Light sleep timed mode *can* be enabled, but effectively forces all SDK timers to be expunged.
//
// Null mode turns off radio, no need for extra code to enable MODEM sleep after switching to NULL mode.

extern "C" bool fpm_is_open(void);
extern "C" bool fpm_rf_is_closed(void);

// We have to wait for certain {F,}PM changes that happen in SDK system idle task
template <typename T>
bool wait_for_fpm(duration::Milliseconds timeout, T&& condition) {
    return time::blockingDelay(
        timeout, duration::Milliseconds{ 1 }, std::forward<T>(condition));
}

template <typename Condition, typename Action>
bool wait_for_fpm(duration::Milliseconds timeout, Condition&& condition, Action&& action) {
    if (condition()) {
        action();
        return wait_for_fpm(timeout, std::forward<Condition>(condition));
    }

    return false;
}

bool forced_wakeup() {
    // Per API spec, we have to disable current forced PM mode to switch
    // it to something else. Wait for a bit until both conditions are true
    // and no idle task is attached to the system loop.
    constexpr auto Timeout = duration::Seconds{ 1 };
    const auto rf_closed = wait_for_fpm(
        Timeout, fpm_rf_is_closed, wifi_fpm_do_wakeup);
    if (rf_closed) {
        return false;
    }

    const auto fpm_opened = wait_for_fpm(
        Timeout, fpm_is_open, wifi_fpm_close);
    if (fpm_opened) {
        return false;
    }

    return true;
}

// Generic PM mode that disables RF peripheral.
// We can still execute user code while it is active.
bool forced_modem_sleep() {
    if (!wifiDisabled()) {
        return false;
    }

    if (!forced_wakeup()) {
        return false;
    }

    wifi_fpm_set_sleep_type(MODEM_SLEEP_T);
    wifi_fpm_open();

    const auto result = wifi_fpm_do_sleep(FpmSleepIndefinite.count());
    if (result != 0) {
        wifi_fpm_close();
        return false;
    }

    // Change *would* occur regardless, but we still notify that expected t/o happened
    return wait_for_fpm(
        duration::Milliseconds{ 1000 },
        []() {
            return !fpm_rf_is_closed();
        });
}

// CPU + RF power saving mode.
// SDK enters IDLE task with minimal amount of operations.
// User code would not be executed while the device is asleep.
// On wakeup, execution resumes from this point.
struct FpmLightSleep {
    FpmLightSleep();
    ~FpmLightSleep();

    explicit operator bool() const {
        return _ok;
    }

private:
    bool _ok { false };
};

FpmLightSleep::~FpmLightSleep() {
    if (_ok) {
        wifi_fpm_close();
        system::run_after_sleep();
    }

    wifi_fpm_auto_sleep_set_in_null_mode(1);
    espurnaReload();
    forced_modem_sleep();
}

FpmLightSleep::FpmLightSleep() {
    // Unlike deep sleep option, we have to manually go over everything related
    // to WiFi state management; both for our internals and SDK ones
    wifi_fpm_auto_sleep_set_in_null_mode(0);

    // Since both sleep variants are going to block user
    // task, WiFi actions would be delayed until woken up
    wifiDisconnect();
    wifiDisable();

    if (!forced_wakeup()) {
        return;
    }

    // ...before FPM type can be changed yet again
    wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
    wifi_fpm_open();

    system::run_before_sleep();

    _ok = true;
}

bool forced_light_sleep(sleep::Microseconds time) {
    // Can't really do what deep sleep does without EXT wakeup source
    if ((time <= FpmSleepMin) || (time >= FpmSleepIndefinite)) {
        return false;
    }

    // Common before and after actions
    FpmLightSleep sleep;
    if (!sleep) {
        return false;
    }

    // NONOS has a quirky implementation - while RF peripheral is stopped,
    // neither system or sdk tasks are. Timers are still processed,
    // interrupts are happening, background tasks may still execute.
    //
    // Instead of doing a (fairly common) workaround that temporarily hides
    // every available timer from SDK, just pretend this works as intended and
    // block with software timer loop.
    //
    // Also ref. `ets_set_idle_cb` processing at 0x3fffdab0 (func) and 0x3fffdab4 (arg)
    // (in case RF + CPU sleep and RTC peripheral communication can be replicated here)
    static bool block;
    block = true;

    wifi_fpm_set_wakeup_cb([]() {
        block = false;
    });

    const auto result = wifi_fpm_do_sleep(time.count());
    if (result == 0) {
        const auto Wait = std::chrono::duration_cast<duration::Milliseconds>(time);
        espurna::time::blockingDelay(
            Wait,
            Wait,
            []() {
                return block;
            });
    }

    wifi_fpm_close();

    return result == 0;
}

bool forced_light_sleep(uint8_t pin, Interrupt interrupt) {
    if (pin == DeepSleepWakeupPin) {
        return false;
    }

    const auto& hardware = hardwareGpio();
    if (!hardware.valid(pin)) {
        return false;
    }

    // Common before and after actions
    FpmLightSleep sleep;
    if (!sleep) {
        return false;
    }

    // TODO: does pin mode apply interrupt mask for wakeup properly?
    // TODO: check whether pin is already in use?
    const auto pin_mode = espurna::gpio::pin_mode(pin);
    pinMode(pin,
        (interrupt == Interrupt::Low)
            ? INPUT
            : INPUT_PULLUP);

    wifi_enable_gpio_wakeup(pin,
        (interrupt == Interrupt::Low)
            ? GPIO_PIN_INTR_LOLEVEL
            : GPIO_PIN_INTR_HILEVEL);

    // User task is suspended for the duration of the sleep,
    // delay is just a context switch so idle task does its job
    const auto result = wifi_fpm_do_sleep(FpmSleepIndefinite.count());
    delay(10);

    // Restore everything back as it was before
    wifi_disable_gpio_wakeup();
    pinMode(pin, pin_mode.value);

    return result == 0;
}

// Extended sleep variant that disables everything but RTC memory.
// Unlike LIGHT sleep, device would be reset via RST pin to wake up.
bool deep_sleep(sleep::Microseconds time) {
    const auto& hardware = hardwareGpio();
    if (hardware.lock(DeepSleepWakeupPin)) {
        return false;
    }

    system_deep_sleep_set_option(RF_DEFAULT);
    if (system_deep_sleep(time.count())) {
        system::run_before_sleep();

        yield();
        return true;
    }

    return false;
}

// Force WiFi RF peripheral to power down when NULL opmode is selected
void init() {
    wifi_fpm_auto_sleep_set_in_null_mode(1);
}

} // namespace
} // namespace sleep

// -----------------------------------------------------------------------------

namespace system {

uint32_t RandomDevice::operator()() const {
    // Repeating SDK source, XORing some ADC-based noise and a HW register exposing the random generator
    // - https://github.com/espressif/ESP8266_RTOS_SDK/blob/d45071563cebe9ca520cbed2537dc840b4d6a1e6/components/esp8266/source/hw_random.c
    // - disassembled source of the `os_random` -> `r_rand` -> `phy_get_rand`
    //   (and avoiding these two additional `call`s)

    // aka WDEV_COUNT_REG, base address
    static constexpr uintptr_t BaseAddress { 0x3ff20c00 };
    // aka WDEV_RAND, the actual register address
    static constexpr uintptr_t Address  { BaseAddress + 0x244 };

    return adc_rand_noise ^ *(reinterpret_cast<volatile uint32_t*>(Address));
}

namespace chip {

// efuse, same bytes as the factory STA MAC
bool mac(uint8_t* out) {
    const uint32_t regs[3] {
        READ_PERI_REG(0x3ff00050),
        READ_PERI_REG(0x3ff00054),
        READ_PERI_REG(0x3ff0005c)};

    out[0] = 0xff;
    out[1] = 0xff;
    out[2] = 0xff;
    out[3] = static_cast<uint8_t>((regs[1] >> 8ul) & 0xfful);
    out[4] = static_cast<uint8_t>(regs[1] & 0xffu);
    out[5] = static_cast<uint8_t>((regs[0] >> 24ul) & 0xffu);

    if (out[2] != 0) {
        out[0] = (regs[2] >> 16ul) & 0xffu;
        out[1] = (regs[2] >> 8ul) & 0xffu;
        out[2] = (regs[2] & 0xffu);
    } else if (0 == ((regs[1] >> 16ul) & 0xff)) {
        out[0] = 0x18;
        out[1] = 0xfe;
        out[2] = 0x34;
    } else if (1 == ((regs[1] >> 16ul) & 0xff)) {
        out[0] = 0xac;
        out[1] = 0xd0;
        out[2] = 0x74;
    }

    return true;
}

bool light_sleep(bool timer, sleep::Microseconds time, int pin, sleep::Interrupt interrupt) {
    if (timer) {
        return ::espurna::sleep::forced_light_sleep(time);
    }

    return ::espurna::sleep::forced_light_sleep(static_cast<uint8_t>(pin), interrupt);
}

bool deep_sleep(sleep::Microseconds time) {
    return ::espurna::sleep::deep_sleep(time);
}

void early_setup() {
#if MODULES_SUPPORT
    modulesMigrateSettings();
#endif
}

} // namespace chip
} // namespace system

// The Core crash handler writes the EEPROM record (ref. custom_crash_callback() of crash.cpp)
namespace debug {
namespace crash {
namespace chip {

bool dump(Print&, bool) {
    return false;
}

bool dump_empty(Print&) {
    return false;
}

void setup() {
}

} // namespace chip
} // namespace crash
} // namespace debug

namespace timer {

constexpr SystemTimer::Duration SystemTimer::DurationMin;
constexpr SystemTimer::Duration SystemTimer::DurationMax;

SystemTimer::SystemTimer() = default;

void SystemTimer::start(duration::Milliseconds duration, Callback callback, bool repeat) {
    stop();
    if (!duration.count()) {
        return;
    }

    if (!_timer) {
        _timer.reset(new os_timer_t{});
    }
    _armed = _timer.get();

    _callback = std::move(callback);
    _repeat = repeat;

    os_timer_setfn(_timer.get(),
        [](void* arg) {
            reinterpret_cast<SystemTimer*>(arg)->callback();
        },
        this);

    size_t total = 0;
    if (duration > DurationMax) {
        total = 1;
        while (duration > DurationMax) {
            total *= 2;
            duration /= 2;
        }
        _tick.reset(new Tick{
            .total = total,
            .count = 0,
        });
        repeat = true;
    }

    os_timer_arm(_armed, duration.count(), repeat);
}

void SystemTimer::stop() {
    if (_armed) {
        os_timer_disarm(_armed);
    }
    reset();
}

void SystemTimer::reset() {
    _armed = nullptr;
    _callback = Callback();
    _tick = nullptr;
}

void SystemTimer::callback() {
    if (_tick) {
        ++_tick->count;
        if (_tick->count < _tick->total) {
            return;
        }
    }

    if (_repeat) {
        if (_tick) {
            _tick->count = 0;
        }

        _callback();
        return;
    }

    // One-shot is no longer armed by the time its callback runs (same as esp32).
    // Callback is allowed to stop() / re-arm / destroy its own timer, including the
    // callable itself, so work on a local copy and never touch `this` after it returns.
    Callback callback;
    callback.swap(_callback);
    stop();

    callback();
}

void SystemTimer::schedule_once(Duration duration, Callback callback) {
    once(duration, [callback]() {
        espurnaRegisterOnce(callback);
    });
}

} // namespace timer

namespace {

namespace memory {

// esp8266 normally only has a one single heap area, located in DRAM just 'before' the SYS stack
// since Core 3.x.x, internal C-level allocator was extended to support multiple contexts
// - external SPI RAM chip (but, this may not work with sizes above 65KiB on older Cores, check the actual version)
// - part of the IRAM, which will be specifically excluded from the CACHE by using a preprocessed linker file
//
// API expects us to use the same C API as usual - malloc, realloc, calloc, etc.
// Only now we are able to switch 'contexts' and receive different address range, currenty via `umm_{push,pop}_heap(ID)`
// (e.g. UMM_HEAP_DRAM, UMM_HEAP_IRAM, ... which techically is an implementation detail, and ESP::... methods should be used)
//
// Meaning, what happens below is heavily dependant on the when and why these functions are called

size_t freeHeap() {
    return system_get_free_heap_size();
}

// see https://github.com/esp8266/Arduino/pull/8440
template <typename T>
using HasHeapStatsFixBase = decltype(std::declval<T>().getHeapStats(
    std::declval<uint32_t*>(), std::declval<uint32_t*>(), std::declval<uint8_t*>()));

template <typename T>
using HasHeapStatsFix = is_detected<HasHeapStatsFixBase, T>;

template <typename T>
HeapStats heapStats(T& instance, std::true_type) {
    HeapStats out;
    instance.getHeapStats(&out.available, &out.usable, &out.fragmentation);
    return out;
}

template <typename T>
HeapStats heapStats(T& instance, std::false_type) {
    HeapStats out;
    uint16_t usable{0};
    instance.getHeapStats(&out.available, &usable, &out.fragmentation);
    out.usable = usable;
    return out;
}

HeapStats heapStats() {
    return heapStats(ESP, HasHeapStatsFix<EspClass>{});
}

} // namespace memory

namespace boot {

void pre() {
    // Some magic to allow seamless Tasmota OTA upgrades
    // - inject dummy data sequence that is expected to hold current version info
    // - purge settings, since we don't want accidentaly reading something as a kv
    // - sometimes we cannot boot b/c of certain SDK params, purge last 16KiB
    {
        // ref. `SetOption78 1` in Tasmota
        // - https://tasmota.github.io/docs/Commands/#setoptions (> SetOption78   Version check on Tasmota upgrade)
        // - https://github.com/esphome/esphome/blob/0e59243b83913fc724d0229514a84b6ea14717cc/esphome/core/esphal.cpp#L275-L287 (the original idea from esphome)
        // - https://github.com/arendst/Tasmota/blob/217addc2bb2cf46e7633c93e87954b245cb96556/tasmota/settings.ino#L218-L262 (specific checks, which succeed when finding 0xffffffff as version)
        // - https://github.com/arendst/Tasmota/blob/0dfa38df89c8f2a1e582d53d79243881645be0b8/tasmota/i18n.h#L780-L782 (constants)
        volatile uint32_t magic[] __attribute__((unused)) {
            0x5aa55aa5,
            0xffffffff,
            0xa55aa55a,
        };

        // ref. https://github.com/arendst/Tasmota/blob/217addc2bb2cf46e7633c93e87954b245cb96556/tasmota/settings.ino#L24
        // We will certainly find these when rebooting from Tasmota. Purge SDK as well, since we may experience WDT after starting up the softAP
        auto* rtcmem = reinterpret_cast<volatile uint32_t*>(RTCMEM_ADDR);
        if ((0xA55A == rtcmem[64]) && (0xA55A == rtcmem[68])) {
            DEBUG_MSG_P(PSTR("[MAIN] TASMOTA OTA, resetting...\n"));
            rtcmem[64] = rtcmem[68] = 0;
            customResetReason(CustomResetReason::Factory);
            resetSettings();
            forceEraseSDKConfig();
            __builtin_unreachable();
        }

        // TODO: also check for things throughout the flash sector, somehow?
    }

    // Workaround for SDK changes between 1.5.3 and 2.2.x or possible
    // flash corruption happening to the 'default' WiFi config
#if SYSTEM_CHECK_ENABLED
    if (!systemCheck()) {
        const uint32_t Address { ESP.getFlashChipSize() - (FLASH_SECTOR_SIZE * 3) };

        static constexpr size_t PageSize { 256 };
#ifdef FLASH_PAGE_SIZE
        static_assert(FLASH_PAGE_SIZE == PageSize, "");
#endif
        static constexpr auto Alignment = alignof(uint32_t);
        alignas(Alignment) std::array<uint8_t, PageSize> page;

        if (ESP.flashRead(Address, reinterpret_cast<uint32_t*>(page.data()), page.size())) {
            constexpr uint32_t ConfigOffset { 0xb0 };

            // In case flash was already erased at some point, but we are still here
            alignas(Alignment) const std::array<uint8_t, 8> Empty { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            if (std::memcpy(&page[ConfigOffset], &Empty[0], Empty.size()) != 0) {
                return;
            }

            // 0x00B0:  0A 00 00 00 45 53 50 2D XX XX XX XX XX XX 00 00     ESP-XXXXXX
            alignas(Alignment) const std::array<uint8_t, 8> Reference { 0xa0, 0x00, 0x00, 0x00, 0x45, 0x53, 0x50, 0x2d };
            if (std::memcmp(&page[ConfigOffset], &Reference[0], Reference.size()) != 0) {
                DEBUG_MSG_P(PSTR("[MAIN] Invalid SDK config at 0x%08X, resetting...\n"), Address + ConfigOffset);
                customResetReason(CustomResetReason::Factory);
                systemForceStable();
                forceEraseSDKConfig();
                __builtin_unreachable();
            }
        }
    }
#endif
}

} // namespace boot

// raw reboot call, effectively:
// ```
// system_restart();
// esp_suspend();
// ```
// triggered in SYS, might not always result in a clean reboot b/c of expected suspend
// triggered in CONT *should* end up never returning back and loop might now be needed
[[noreturn]] void reset() {
    ESP.restart();
    __builtin_trap();
}

// SDK reserves last 16KiB on the flash for it's own means
// Notice that it *may* also be required to soft-crash the board,
// so it does not end up restoring the configuration cached in RAM
// ref. https://github.com/esp8266/Arduino/issues/1494
bool eraseSDKConfig() {
    return ESP.eraseConfig();
}

[[noreturn]] void forceEraseSDKConfig() {
    eraseSDKConfig();
    __builtin_trap();
}

} // namespace

// Common system module hooks (ref. system.h)
namespace system {
namespace arch {

void pre_setup() {
    ::espurna::boot::pre();
    ::espurna::sleep::init();
}

void setup() {
}

void loop() {
}

// system_get_rst_info() result is cached by the Core init for internal use
uint32_t reset_reason() {
    return resetInfo.reason;
}

[[noreturn]] void restart() {
    ::espurna::reset();
}

} // namespace arch
} // namespace system
} // namespace espurna

// -----------------------------------------------------------------------------

HeapStats systemHeapStats() {
    return espurna::memory::heapStats();
}

size_t systemFreeHeap() {
    return espurna::memory::freeHeap();
}

void reset() {
    espurna::reset();
}

bool eraseSDKConfig() {
    return espurna::eraseSDKConfig();
}

[[noreturn]] void forceEraseSDKConfig() {
    espurna::forceEraseSDKConfig();
    __builtin_unreachable();
}

bool prepareModemForcedSleep() {
    return espurna::sleep::forced_modem_sleep();
}

bool wakeupModemForcedSleep() {
    return espurna::sleep::forced_wakeup();
}

