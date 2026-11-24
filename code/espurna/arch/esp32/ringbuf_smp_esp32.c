// ESP-IDF's ring buffer (esp_ringbuf/ringbuf.c: the UART driver's RX / TX buffers) wakes and blocks tasks with the
// kernel's event list calls under its own spinlock: portENTER_CRITICAL(&mux) / portENTER_CRITICAL_ISR(&mux). On the
// FreeRTOS 11 SMP kernel that lock is the port's IDF critical (port_uxCriticalNestingIDF), not the
// kernel's: no kernel ISR lock, no critical nesting in the TCB. xTaskRemoveFromEventList() -> prvYieldForTask() asserts
// on that (tasks.c:894, "uxCriticalNesting > 0U"), from the UART interrupt as soon as a task waits for TX space (Matter's
// log at its start, on every boot after MATTER.WINDOW), and the ready / delayed lists are changed without the lock the
// other core takes. ringbuf.c is the only ESP-IDF component calling these outside the kernel.
//
// esp_ringbuf is compiled with its two calls renamed to these (espurna/CMakeLists.txt; nothing else of the link calls
// them, the kernel's own queue.c and tasks.c keep the plain ones), in IRAM as ringbuf.c is: the kernel's critical around
// each. The ISR pair in task context too: it masks interrupts, takes the ISR lock and counts the nesting, restores the
// mask it found and never yields or re-enables interrupts under the ring buffer's spinlock (vTaskExitCritical() would do
// both). A yield the wake asks for stays pending (prvYieldForTask), as ringbuf.c's own portYIELD_WITHIN_API() /
// *pxHigherPriorityTaskWoken expect. The kernel is always the SMP one (FreeRTOS 11, sdkconfig.defaults)

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t espurna_ringbuf_xTaskRemoveFromEventList(const List_t* const event_list);
void espurna_ringbuf_vTaskPlaceOnEventList(List_t* const event_list, const TickType_t ticks);

IRAM_ATTR BaseType_t espurna_ringbuf_xTaskRemoveFromEventList(const List_t* const event_list) {
    const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    const BaseType_t out = xTaskRemoveFromEventList(event_list);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    return out;
}

IRAM_ATTR void espurna_ringbuf_vTaskPlaceOnEventList(List_t* const event_list, const TickType_t ticks) {
    const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    vTaskPlaceOnEventList(event_list, ticks);
    taskEXIT_CRITICAL_FROM_ISR(saved);
}
