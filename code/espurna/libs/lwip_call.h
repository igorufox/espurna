/*

LWIP CALL - a function run in lwIP's own context (the raw API: pcbs, groups, netif lists only there), waited for.
Used by libs/tcp_impl.h and the mDNS responder (libs/mdns_responder_impl.h)

*/

#pragma once

#include <type_traits>

#include <lwip/opt.h>
#if !NO_SYS
// FreeRTOS itself: the ESP32 safeboot is plain ESP-IDF, no Arduino
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <FreeRTOS.h>
#include <task.h>
#endif
#include <lwip/sys.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#endif

namespace espurna {
namespace lwip {

#if NO_SYS

// no OS: the loop is lwIP's context
template <typename F>
void lwipCall(F&& func) {
    func();
}

#else

// The semaphore a task waits on for its calls: one per calling task, made once (the loop, the Matter task, ...)
inline sys_sem_t* lwipCallSemaphore() {
    struct Slot {
        TaskHandle_t task;
        sys_sem_t sem;
    };
    static Slot slots[6] {};

    const auto self = xTaskGetCurrentTaskHandle();
    for (auto& slot : slots) {
        if (slot.task == self) {
            return &slot.sem;
        }
    }

    sys_sem_t sem;
    if (sys_sem_new(&sem, 0) != ERR_OK) {
        return nullptr;
    }

    // the SDKs differ here only: ESP-IDF (two cores) a spinlock of both cores; the BK72xx (one core) no other task
    // while the scheduler is suspended (its taskENTER_CRITICAL() needs the SDK's own declarations)
#if defined(ESP32)
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#define LWIP_CALL_ENTER() taskENTER_CRITICAL(&mux)
#define LWIP_CALL_EXIT() taskEXIT_CRITICAL(&mux)
#else
#define LWIP_CALL_ENTER() vTaskSuspendAll()
#define LWIP_CALL_EXIT() xTaskResumeAll()
#endif
    LWIP_CALL_ENTER();
    for (auto& slot : slots) {
        if (!slot.task) {
            slot.task = self;
            slot.sem = sem;
            LWIP_CALL_EXIT();
            return &slot.sem;
        }
    }
    LWIP_CALL_EXIT();
#undef LWIP_CALL_ENTER
#undef LWIP_CALL_EXIT

    sys_sem_free(&sem);
    return nullptr;
}

template <typename F>
struct LwipCall {
    tcpip_api_call_data base;   // first: lwIP passes this pointer back
    F* func;
    sys_sem_t* done;
};

// Runs `func` in the tcpip thread and waits for it. Always a message to that thread, never lwIP's core lock taken in
// the calling task: with LWIP_TCPIP_CORE_LOCKING (LibreTiny's lwIP, the default of lwIP) tcpip_api_call() locks the
// core in the caller and runs `func` there, and the loop then sent from its own task with the core locked - the
// tcpip thread waited for the loop for 8.7 s and the watchdog reset the Beken (W1, 10.0.5.139 2026-11-14, the
// stall record: `main` in sys_mutex_lock under tcpip_api_call, priority inherited from tcp/ip). The ESP32 (core
// locking off in ESP-IDF) always did it this way
template <typename F>
void lwipCall(F&& func) {
    using Func = typename std::remove_reference<F>::type;
    LwipCall<Func> call {};
    call.func = &func;
    call.done = lwipCallSemaphore();

    const auto run = [](void* arg) {
        auto* data = reinterpret_cast<LwipCall<Func>*>(arg);
        (*data->func)();
        sys_sem_signal(data->done);
    };

    if (call.done && (tcpip_callback(run, &call) == ERR_OK)) {
        sys_arch_sem_wait(call.done, 0);
        return;
    }

    // no semaphore or no message left: lwIP's own way
    tcpip_api_call([](tcpip_api_call_data* data) -> err_t {
        (*reinterpret_cast<LwipCall<Func>*>(data)->func)();
        return ERR_OK;
    }, &call.base);
}

#endif

} // namespace lwip
} // namespace espurna
