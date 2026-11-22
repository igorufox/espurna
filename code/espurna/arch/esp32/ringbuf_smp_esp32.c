// ESP-IDF's ring buffer (esp_ringbuf/ringbuf.c: the UART driver's RX / TX buffers) wakes and blocks tasks with the
// kernel's event list calls under its own spinlock: portENTER_CRITICAL(&mux) / portENTER_CRITICAL_ISR(&mux). On the
// FreeRTOS 11 SMP kernel (CONFIG_FREERTOS_SMP) that lock is the port's IDF critical (port_uxCriticalNestingIDF), not the
// kernel's: no kernel ISR lock, no critical nesting in the TCB. xTaskRemoveFromEventList() -> prvYieldForTask() asserts
// on that (tasks.c:894, "uxCriticalNesting > 0U"), from the UART interrupt as soon as a task waits for TX space (Matter's
// log at its start, on every boot after MATTER.WINDOW), and the ready / delayed lists are changed without the lock the
// other core takes. ringbuf.c is the only ESP-IDF component calling these outside the kernel.
//
// The linker sends ringbuf.c's calls here (-Wl,--wrap in espurna/CMakeLists.txt; the kernel's own calls inside tasks.c
// are not wrapped), in IRAM as ringbuf.c is: the kernel's critical around each. The ISR pair in task context too: it
// masks interrupts, takes the ISR lock and counts the nesting, restores the mask it found and never yields or re-enables
// interrupts under the ring buffer's spinlock (vTaskExitCritical() would do both). A yield the wake asks for stays
// pending (prvYieldForTask), as ringbuf.c's own portYIELD_WITHIN_API() / *pxHigherPriorityTaskWoken expect.

#include "sdkconfig.h"

#if CONFIG_FREERTOS_SMP

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t __real_xTaskRemoveFromEventList(const List_t* const event_list);
void __real_vTaskPlaceOnEventList(List_t* const event_list, const TickType_t ticks);

IRAM_ATTR BaseType_t __wrap_xTaskRemoveFromEventList(const List_t* const event_list) {
    const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    const BaseType_t out = __real_xTaskRemoveFromEventList(event_list);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    return out;
}

IRAM_ATTR void __wrap_vTaskPlaceOnEventList(List_t* const event_list, const TickType_t ticks) {
    const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    __real_vTaskPlaceOnEventList(event_list, ticks);
    taskEXIT_CRITICAL_FROM_ISR(saved);
}

#endif
