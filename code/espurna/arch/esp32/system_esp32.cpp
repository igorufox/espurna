/*

SYSTEM MODULE FOR ESP32

Chip part, the FreeRTOS chips part is in arch/rtos/system_rtos.cpp, common part is in system.cpp

*/

#include "../../espurna_tasks.h"

#include <esp_system.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>    // ESP-IDF 5 (core 3.x)
#endif
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_phy_init.h>
#include <driver/gpio.h>
#include <esp_core_dump.h>

#include <iterator>
#include <memory>
#include <new>

#include "../../espurna.h"
#include "../../rtcmem.h"
#include "../../crash.h"

// enableLoopWDT(), loopTaskWDTEnabled: whether the loop task is watched by the task WDT (core_esp32.cpp)
#include "core_esp32.h"

#if TERMINAL_SUPPORT
void coredumpSetup();
#endif

// -----------------------------------------------------------------------------
// MEMORY, RANDOM
// -----------------------------------------------------------------------------

size_t systemFreeHeap() {
    return esp_get_free_heap_size();
}

size_t systemHeapMinimum() {
    return esp_get_minimum_free_heap_size();
}

// byte addressable heap (systemHeapStats(), arch/rtos/system_rtos.cpp)
void espurna::system::chip::sdk::heap(uint32_t& free, uint32_t& block) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    free = info.total_free_bytes;
    block = info.largest_free_block;
}

uint32_t espurna::system::RandomDevice::operator()() const {
    return esp_random();
}

// -----------------------------------------------------------------------------
// SDK CONFIG
// -----------------------------------------------------------------------------

// esp8266 erases the SDK area, i.e. stored WiFi config and RF calibration.
// ESP32 keeps both in NVS, separately from our settings.
bool eraseSDKConfig() {
    const auto wifi = esp_wifi_restore();
    const auto phy = esp_phy_erase_cal_data_in_nvs();
    return (wifi == ESP_OK) && (phy == ESP_OK);
}

namespace espurna {
namespace system {

// Two cores: a spinlock keeps the other one out (and masks the interrupts of this one). Nests
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

// The loop stall watcher is common (arch/rtos/system_rtos.cpp, ref. system_chip.h). Here what it leaves for after the
// reset: the task WDT panics (CONFIG_ESP_TASK_WDT_PANIC) and the core dump has every task, RTC memory keeps which loop
// callback the loop task stood in, for `info` after the reboot
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

// Same as esp8266 delay(), the loop task itself only feeds it between loop() calls
void sdk::feed_loop_wdt() {
    if (loopTaskWDTEnabled) {
        esp_task_wdt_reset();
    }
}

[[noreturn]] void sdk::reboot() {
    // Same as esp8266 hardwareGpioIgnore(), relay outputs keep their state through the software
    // reset instead of dropping for the whole boot. Pads are latched here and released on the
    // first write after the boot, when the relay module already restored the same state.
    // (ref. gpio_esp32.cpp HardwarePin)
    if (Rtcmem) {
        const uint64_t ignore = Rtcmem->gpio_ignore;
        for (uint8_t pin = 0; pin < GPIO_NUM_MAX; ++pin) {
            if (ignore & (uint64_t{1} << pin)) {
                gpio_hold_en(static_cast<gpio_num_t>(pin));
            }
        }
    }

    esp_restart();
    __builtin_unreachable();
}

bool mac(uint8_t* out) {
    return esp_read_mac(out, ESP_MAC_WIFI_STA) == ESP_OK;
}

// Radio is powered down during both light and deep sleep,
// WiFi is stopped before and restarted after light sleep (and the FSM reconnects)
bool light_sleep(bool timer, sleep::Microseconds time, int pin, sleep::Interrupt interrupt) {
    if (timer) {
        if ((time <= sleep::FpmSleepMin) || (time >= sleep::FpmSleepIndefinite)) {
            return false;
        }
        esp_sleep_enable_timer_wakeup(time.count());
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
        esp_sleep_enable_timer_wakeup(us);
        esp_deep_sleep_start();
    });

    return true;
}

void early_setup() {
}

void sdk::setup() {
#if TERMINAL_SUPPORT
    // COREDUMP / COREDUMP.ERASE terminal commands, ref. coredump_esp32.cpp
    coredumpSetup();
#endif

    stall::setup();
}

// task WDT, CONFIG_ESP_TASK_WDT_TIMEOUT_S (5s)
void sdk::enable_loop_wdt() {
    enableLoopWDT();
    stall::period(CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000);
}

} // namespace chip

namespace chip {

void pre_setup() {
    // The TCP servers (libs/tcp.h: web, telnet) are started before WiFi is, make sure the TCP/IP stack is up
    esp_netif_init();

    // analogRead() is 0...1023 already, the range of the esp8266 ADC (core_esp32.cpp)
}

// Same values as esp8266, common code (e.g. the stability check) relies on them
uint32_t reset_reason() {
    switch (esp_reset_reason()) {
    case ESP_RST_EXT:
        return REASON_EXT_SYS_RST;
    case ESP_RST_SW:
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

// Nothing writes the EEPROM record here, panic info is saved in the core dump partition instead
// (ref. arch/esp32/coredump_esp32.cpp)
namespace espurna {
namespace debug {
namespace crash {
namespace chip {

bool dump(::espurna::Output&, bool) {
    return false;
}

bool dump_empty(::espurna::Output& print) {
    print.printf_P(PSTR("Reason of restart: %s\nCrash details (if any) are in the core dump, see COREDUMP\n"),
        ESP_COMPAT.getResetReason().c_str());
    return true;
}

// the core dump in short: the task, its pc and the backtrace (the page looks for a module there)
bool cause(CrashCause& out) {
    size_t address { 0 };
    size_t size { 0 };
    if (esp_core_dump_image_get(&address, &size) != ESP_OK) {
        return true;
    }

    std::unique_ptr<esp_core_dump_summary_t> summary(new (std::nothrow) esp_core_dump_summary_t);
    if (!summary || (esp_core_dump_get_summary(summary.get()) != ESP_OK)) {
        return true;
    }

    snprintf_P(out.text, sizeof(out.text), PSTR("core dump: task %.16s, pc 0x%08X, exccause %u"),
        summary->exc_task, summary->exc_pc, summary->ex_info.exc_cause);
    for (uint32_t index = 0; (index < summary->exc_bt_info.depth) && (out.count < std::size(out.addresses)); ++index) {
        out.addresses[out.count++] = summary->exc_bt_info.bt[index];
    }

    return true;
}

void setup() {
}

} // namespace chip
} // namespace crash
} // namespace debug
} // namespace espurna

// compat_esp32.cpp, ESP_COMPAT.getResetInfo()
espurna::string systemStallInfo() {
    return espurna::system::stall::last;
}
