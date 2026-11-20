/*

SYSTEM MODULE FOR ESP8266 ON ESP8266_RTOS_SDK 3.4

Chip part, the FreeRTOS chips part is in arch/rtos/system_rtos.cpp, common part is in system.cpp

*/


#include "../../espurna_tasks.h"

#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_freertos_hooks.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <tcpip_adapter.h>

#include "espurna.h"
#include "rtcmem.h"
#include "crash.h"

// -----------------------------------------------------------------------------
// MEMORY, RANDOM
// -----------------------------------------------------------------------------

size_t systemFreeHeap() {
    return esp_get_free_heap_size();
}

size_t systemHeapMinimum() {
    return esp_get_minimum_free_heap_size();
}

// the free heap and its largest block, both of the same capabilities (heap_caps_get_largest_free_block: ESPurna's
// addition to the SDK package; before it the block was 0 and the whole free heap counted as one block)
void espurna::system::chip::sdk::heap(uint32_t& free, uint32_t& block) {
    free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

uint32_t espurna::system::RandomDevice::operator()() const {
    return esp_random();
}

// -----------------------------------------------------------------------------
// SDK CONFIG
// -----------------------------------------------------------------------------

// the stored Wi-Fi configuration of the SDK (NVS); the RF calibration stays
bool eraseSDKConfig() {
    return esp_wifi_restore() == ESP_OK;
}

namespace espurna {
namespace system {

// One core: the FreeRTOS critical section of the port (interrupts off). Nests
uint32_t criticalEnter() {
    portENTER_CRITICAL();
    return 0;
}

void criticalExit(uint32_t) {
    portEXIT_CRITICAL();
}

// The loop stall watcher is common (arch/rtos/system_rtos.cpp, ref. system_chip.h). Here what it leaves for after the
// reset: RTC memory keeps which loop callback the loop task stood in, for `info` after the reboot
namespace stall {
namespace {

constexpr uint32_t Magic { 0x4C4F4F50 }; // LOOP

struct Record {
    uint32_t magic;
    Report report;
};

RTC_NOINIT_ATTR Record record;

espurna::string last;

// a stall from the previous boot, once: what loop() stood in (`addr2line` it on the build's .elf)
void setup() {
    if (record.magic == Magic) {
        switch (esp_reset_reason()) {
        case ESP_RST_PANIC:
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT: {
            char buf[112];
            snprintf_P(buf, sizeof(buf), PSTR("loop() stalled %u ms (watchdog %u ms) in 0x%08X at uptime %u s"),
                record.report.since, record.report.period, record.report.callback, record.report.uptime / 1000);
            last = buf;
            break;
        }
        default:
            break;
        }
    }

    record.magic = 0;
}

} // namespace

namespace chip {

void record(const Report& report) {
    ::espurna::system::stall::record = Record{Magic, report};
}

void cancel() {
    ::espurna::system::stall::record.magic = 0;
}

[[noreturn]] void reset(const Report& report) {
    record(report);
    abort();
}

} // namespace chip
} // namespace stall

namespace chip {

// The hardware watchdog of the SDK is fed by its idle task (CONFIG_TASK_WDT): it watches that every task gives the CPU
// away, the loop task itself is watched by the stall watcher (no loop watchdog period here)
void sdk::feed_loop_wdt() {
}

// the restart of core_esp8266.cpp: esp_restart() without its Wi-Fi stop (that crashed the scheduler, 11-18)
[[noreturn]] void sdk::reboot() {
    ::espurna::core::chip::restart();
    __builtin_unreachable();
}

bool mac(uint8_t* out) {
    return esp_read_mac(out, ESP_MAC_WIFI_STA) == ESP_OK;
}

// Radio is powered down during light sleep, Wi-Fi is stopped before and restarted after it (and the FSM reconnects)
bool light_sleep(bool timer, sleep::Microseconds time, int pin, sleep::Interrupt interrupt) {
    if (timer) {
        if ((time <= sleep::FpmSleepMin) || (time >= sleep::FpmSleepIndefinite)) {
            return false;
        }
        esp_sleep_enable_timer_wakeup(static_cast<uint32_t>(time.count()));
    }

    if (pin >= 0) {
        if (!GPIO_IS_VALID_GPIO(pin)) {
            return false;
        }

        const auto level = (interrupt == sleep::Interrupt::Low)
            ? GPIO_INTR_LOW_LEVEL
            : GPIO_INTR_HIGH_LEVEL;
        gpio_wakeup_enable(static_cast<gpio_num_t>(pin), level);
        esp_sleep_enable_gpio_wakeup();
    }

    run_before_sleep();
    esp_wifi_stop();

    const auto result = esp_light_sleep_start();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (pin >= 0) {
        gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
    }

    esp_wifi_start();
    wifiDisconnect();

    run_after_sleep();

    return result == ESP_OK;
}

bool deep_sleep(sleep::Microseconds time) {
    if (!time.count()) {
        return false;
    }

    run_before_sleep();

    // same as esp8266, sleep starts once the caller returns (so e.g. terminal
    // can still respond), with a short delay for the network buffers to flush
    const uint64_t us = time.count();
    espurnaRegisterOnce([us]() {
        espurna::time::blockingDelay(espurna::duration::Milliseconds(100));
        customResetReason(CustomResetReason::None);
        esp_deep_sleep(us);
    });

    return true;
}

void early_setup() {
}

namespace {

// The tick hook (every FreeRTOS tick, 10 ms, from the tick interrupt): the watcher task's heartbeat must move. A board
// whose tasks the scheduler lost (11-18: on the network, answering nothing, the idle task still feeding the hardware
// watchdog, 10 minutes until a hand pressed RST) resets after 20 s without it; abort() is a panic, the panic restarts
constexpr uint32_t QuietTicksMax { 20000 / portTICK_PERIOD_MS };

// the watcher starts in the setup; a boot that never gets there (11-18: hung right after the Wi-Fi start, no watcher
// yet) resets too
constexpr uint32_t StartTicksMax { 30000 / portTICK_PERIOD_MS };

void watch_tick() {
    static uint32_t seen { 0 };
    static uint32_t quiet { 0 };

    const uint32_t beat = ::espurna::system::stall::beat;
    if (beat != seen) {
        seen = beat;
        quiet = 0;
        return;
    }

    if (++quiet >= (beat ? QuietTicksMax : StartTicksMax)) {
        quiet = 0;
        ::espurna::system::stall::chip::record(::espurna::system::stall::lateReport());
        abort();
    }
}

} // namespace

void sdk::setup() {
    stall::setup();
    esp_register_freertos_tick_hook(watch_tick);
}

// the hardware watchdog runs from the SDK's start (fed by the idle task), ref. feed_loop_wdt()
void sdk::enable_loop_wdt() {
}

} // namespace chip

namespace chip {

void pre_setup() {
    // The TCP servers (libs/tcp.h: web, telnet) are started before WiFi is, make sure the TCP/IP stack is up
    tcpip_adapter_init();
}

// Same values as esp8266, common code (e.g. the stability check) relies on them
uint32_t reset_reason() {
    switch (esp_reset_reason()) {
    case ESP_RST_EXT:
        return REASON_EXT_SYS_RST;
    case ESP_RST_SW:
    case ESP_RST_FAST_SW:
        return REASON_SOFT_RESTART;
    case ESP_RST_PANIC:
        return REASON_EXCEPTION_RST;
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
        return REASON_WDT_RST;
    case ESP_RST_TASK_WDT:
        return REASON_SOFT_WDT_RST;
    case ESP_RST_DEEPSLEEP:
        return REASON_DEEP_SLEEP_AWAKE;
    // power related, not the firmware's fault
    case ESP_RST_POWERON:
    case ESP_RST_BROWNOUT:
    case ESP_RST_SDIO:
    case ESP_RST_UNKNOWN:
        break;
    }

    return REASON_DEFAULT_RST;
}

} // namespace chip
} // namespace system
} // namespace espurna

// The SDK's panic handler prints the registers and the backtrace on the UART, nothing is kept for after the reset
namespace espurna {
namespace debug {
namespace crash {
namespace chip {

bool dump(::espurna::Output&, bool) {
    return false;
}

bool dump_empty(::espurna::Output& print) {
    print.printf_P(PSTR("Reason of restart: %s\n"), ESP_COMPAT.getResetReason().c_str());
    return true;
}

bool cause(CrashCause&) {
    return true;
}

void setup() {
}

} // namespace chip
} // namespace crash
} // namespace debug
} // namespace espurna

// compat_esp8266.cpp, ESP_COMPAT.getResetInfo()
espurna::string systemStallInfo() {
    return espurna::system::stall::last;
}
