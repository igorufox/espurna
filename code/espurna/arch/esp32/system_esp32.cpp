/*

SYSTEM MODULE FOR ESP32

Chip part, the FreeRTOS chips part is in arch/rtos/system_rtos.cpp, common part is in system.cpp

*/

#include <Arduino.h>

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

#include "espurna.h"
#include "rtcmem.h"
#include "crash.h"

// Arduino core main.cpp, whether the loop task is watched by the task WDT
extern bool loopTaskWDTEnabled;

#if TERMINAL_SUPPORT
void coredumpSetup();
#endif

// -----------------------------------------------------------------------------
// MEMORY, RANDOM
// -----------------------------------------------------------------------------

size_t systemFreeHeap() {
    return esp_get_free_heap_size();
}

HeapStats systemHeapStats() {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    uint8_t frag = info.total_free_bytes ? (100 - (info.largest_free_block * 100 / info.total_free_bytes)) : 0;
    return {(uint32_t)info.total_free_bytes, (uint32_t)info.largest_free_block, frag};
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
namespace {

// The task WDT does not see a loop() that waits forever in something calling delay() (which feeds it): a task
// of its own counts the loop passes and aborts when they stop. The panic reboots the chip and writes the core
// dump (every task); RTC memory keeps which loop callback it stood in, for `info` after the reboot.
namespace stall {

#ifndef SYSTEM_LOOP_STALL
#define SYSTEM_LOOP_STALL 60    // seconds without a loop() pass before the reboot, 0 = off
#endif

constexpr uint32_t Seconds { SYSTEM_LOOP_STALL };
constexpr uint32_t Magic { 0x4C4F4F50 }; // LOOP

struct Record {
    uint32_t magic;
    uint32_t callback;
    uint32_t seconds;
    uint32_t uptime;
};

RTC_NOINIT_ATTR Record record;

espurna::system::Shared<uint32_t> passes { 0 };
String last;

void task(void*) {
    uint32_t seen = passes.load();
    uint32_t same = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const auto now = passes.load();
        if (now != seen) {
            seen = now;
            same = 0;
            continue;
        }

        if (++same >= Seconds) {
            record = Record{Magic, static_cast<uint32_t>(espurnaLoopCurrent()), same,
                static_cast<uint32_t>(esp_timer_get_time() / 1000000)};
            abort();
        }
    }
}

// a stall from the previous boot, once: what loop() stood in (`addr2line` it on the build's .elf)
void setup() {
    if ((record.magic == Magic) && (esp_reset_reason() == ESP_RST_PANIC)) {
        char buf[96];
        snprintf_P(buf, sizeof(buf), PSTR("loop() stalled %u s in 0x%08X at uptime %u s"),
            record.seconds, record.callback, record.uptime);
        last = buf;
    }
    record.magic = 0;

    if (Seconds) {
        xTaskCreatePinnedToCore(task, "loopstall", 2048, nullptr, 1, nullptr, 0);
    }
}

} // namespace stall
} // namespace

namespace chip {

void sdk::loop() {
    stall::passes.fetch_add(1);
}

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
        const uint32_t ignore = Rtcmem->gpio_ignore;
        for (uint8_t pin = 0; pin < 32; ++pin) {
            if (ignore & (uint32_t{1} << pin)) {
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
}

} // namespace chip

namespace chip {

void pre_setup() {
    // The TCP servers (libs/tcp.h: web, telnet) are started before WiFi is, make sure the TCP/IP stack is up
    esp_netif_init();

    // Same range as esp8266 ADC (0...1023), analog buttons and sensors expect it
    analogReadResolution(10);
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

bool dump(Print&, bool) {
    return false;
}

bool dump_empty(Print& print) {
    print.printf_P(PSTR("Reason of restart: %s\nCrash details (if any) are in the core dump, see COREDUMP\n"),
        ESP.getResetReason().c_str());
    return true;
}

void setup() {
}

} // namespace chip
} // namespace crash
} // namespace debug
} // namespace espurna

// compat_esp32.cpp, ESP.getResetInfo()
String systemStallInfo() {
    return espurna::system::stall::last;
}
