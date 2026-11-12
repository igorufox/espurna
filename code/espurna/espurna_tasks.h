/*

EXECUTION MODEL

Common code is written for the esp8266 cooperative model, where network callbacks (lwIP, SDK events, SNTP,
Matter) never overlap with loop(). ESPurna's own network code (libs/tcp.h and everything on it) runs in loop() on
every chip, lwIP's callbacks only fill its buffers. Chips running those in tasks of their own provide the
synchronization through the same few names, and on esp8266 every one of them is a direct call or nothing:

- ParallelTasks          true when network callbacks run in parallel with loop()
- loopTask()             whether the caller is the loop task
- Mutex, MutexLock       recursive lock between tasks (FreeRTOS mutex, or nothing)
- Shared<T>              small value read by one task and written by another (critical section, or plain T)
- EventQueue<T, Size>    fixed number of events of another task for loop(), in order (FreeRTOS queue, or a ring
                         buffer filled from SYS). T is copied byte-wise, the full queue drops and counts
- runInLoop()            any callable for loop(), in order, at most RunInLoopMax of them and RunInLoopBytesMax of their
                         copied data (drops and counts the rest)
- registerPendingOutput  restart() waits for the output some module did not send out yet
- TaskQueue<T, Max>      items from other tasks for loop(), at most Max bytes of them

The model is the SDK's, not a choice: the esp8266 NONOS SDK runs network callbacks in SYS, between loop() calls,
the FreeRTOS SDKs (ESP32, LibreTiny) run them in tasks of their own. Below, one part per model. What the chip itself
forces (the critical section: a spinlock of both ESP32 cores, the interrupt mask of the single core BK72xx) is in
arch/rtos/system_rtos.cpp

*/

#pragma once

#include "system.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace espurna {
namespace system {

// Run the callback from loop(), in the order of calls. Safe to call from any task (Wi-Fi events, SNTP, Matter on
// ESP32 / Beken). `bytes` is the data the callback
// carries (a copied message): when loop() falls behind, callbacks are dropped instead of running out of heap (the
// first one queued always goes in). false when dropped, which is counted and reported once per burst
constexpr size_t RunInLoopMax { 64 };
constexpr size_t RunInLoopBytesMax { 8192 };

bool runInLoop(std::function<void()> callback, size_t bytes = 0);

} // namespace system
} // namespace espurna

// the part of the SDK model: arch/esp8266/tasks_chip.h, arch/rtos/tasks_chip.h (ESP32, Beken)
#include "tasks_chip.h"
