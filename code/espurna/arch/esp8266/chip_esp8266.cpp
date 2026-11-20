/*

CHIP DESCRIPTION AND LWIP GLUE, ESP8266 on ESP8266_RTOS_SDK 3.4 (ref. espurna_chip.h)

*/

#include "../../espurna.h"

#include "../../espurna_chip.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <new>

namespace espurna {
namespace chip {

WebInfo web_info() {
    return WebInfo{
        .arch = "esp8266",
        .mcu = "ESP8266EX",
        .layout = nullptr,
        .tcm_used = 0,
        .tcm_size = 0,
    };
}

void info(::espurna::Output& out) {
    out.printf_P(PSTR("mcu: ESP8266EX chipid: %s freq: %umhz\n"),
            systemChipId().c_str(), static_cast<unsigned>(system_get_cpu_freq()));
}

// HEAP: every task, its priority and the bytes of its stack never used (the stacks are most of the RAM of the RTOS
// build: loopTask, the Wi-Fi's ppT, esp_timer, the timer service, the event loop, tcpip, idle)
void memory(::espurna::Output& out) {
    UBaseType_t count = uxTaskGetNumberOfTasks();
    std::unique_ptr<TaskStatus_t[]> tasks(new (std::nothrow) TaskStatus_t[count + 2]);
    if (!tasks) {
        return;
    }

    count = uxTaskGetSystemState(tasks.get(), count + 2, nullptr);
    out.printf_P(PSTR("tasks: %u\n"), static_cast<unsigned>(count));
    for (UBaseType_t index = 0; index < count; ++index) {
        const auto& task = tasks[index];
        // the TCB and the stack too: a panic's registers (an address in the heap) name the task they point into
        out.printf_P(PSTR("  %-16s prio %2u stack free %5u tcb %p stack %p\n"), task.pcTaskName,
            static_cast<unsigned>(task.uxCurrentPriority), static_cast<unsigned>(task.usStackHighWaterMark),
            task.xHandle, task.pxStackBase);
    }
}

// lwIP of the SDK: IPv4 and IPv6 in one ip_addr_t
espurna::Ip4 address(const ip_addr_t* addr) {
    return espurna::Ip4(ip_addr_get_ip4_u32(addr));
}

// Flash is split into the partitions of the SDK's partition table
bool storage(::espurna::Output& out, const char* mode) {
    out.printf_P(PSTR("speed: %u\n"), espurna::core::chip::flash_speed());
    out.printf_P(PSTR("mode: %s\n"), mode);
    out.printf_P(PSTR("size: %u\n"), espurna::core::chip::flash_size());

    const auto* running = esp_ota_get_running_partition();

    // the SDK has no 'any type': the app partitions, then the data ones
    for (const auto type : {ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA}) {
        auto it = esp_partition_find(type, ESP_PARTITION_SUBTYPE_ANY, nullptr);
        for (; it != nullptr; it = esp_partition_next(it)) {
            const auto* part = esp_partition_get(it);
            out.printf_P(PSTR("%-8s [%08X...%08X) (%u bytes)%s\n"),
                part->label, part->address, part->address + part->size, part->size,
                (part == running) ? PSTR(" running") : PSTR(""));
        }
        esp_partition_iterator_release(it);
    }

    out.printf_P(PSTR("app: %u bytes, free for OTA: %u bytes\n"),
        espurna::core::chip::sketch_size(), espurna::core::chip::sketch_space());

    return true;
}

espurna::string image() {
    return ESPURNA_IMAGE;
}

// the heap of the ESP8266 is the NONOS build's size (~40 KB free after start), the same guard as there
WebGuard web_guard() {
    // 19.11: the MODULES page opened its parts in parallel, 4 connections x (TCP send buffer + request) took the heap to
    // 2.4 KB (largest block < 1 KB), the Wi-Fi driver dropped the received frames (pings lost for seconds). One request
    // at a time, a new connection waits while the heap or its largest block is short
    return WebGuard{
        .heap = 12288,
        .block = 4096,
        .lwip = 0,
        .conns = 6,
        .requests = 1,
    };
}

// the SDK, no Arduino core (lib/espurna-core is the layer of the Arduino names)
espurna::string core_version() {
    return espurna::string("ESP8266_RTOS_SDK ") + esp_get_idf_version();
}

espurna::string core_revision() {
    return esp_get_idf_version();
}

} // namespace chip
} // namespace espurna

// utils.h: espurna::Ip4 of the layer (ArduinoCore-API) has no toString()
espurna::string ipToString(const espurna::Ip4& ip) {
    espurna::string out;
    out.reserve(16);
    for (int index = 0; index < 4; ++index) {
        if (index) {
            out += '.';
        }
        out += espurna::string(ip[index]);
    }

    return out;
}
