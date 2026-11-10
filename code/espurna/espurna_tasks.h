/*

EXECUTION MODEL

Common code is written for the esp8266 cooperative model, where network callbacks (lwIP, AsyncTCP,
SDK events, SNTP) never overlap with loop(). Chips running those in tasks of their own provide the
synchronization through the same few names, and on esp8266 every one of them is free (no code, no RAM):

- ParallelTasks          true when network callbacks run in parallel with loop()
- loopTask()             whether the caller is the loop task
- Shared<T>              value read by one task and written by another (std::atomic<T>, or plain T)
- Mutex, MutexLock       lock between tasks (std::mutex + std::lock_guard, or nothing)
- Deferred               ordered hand-over of network events to loop(), when the loop lock could not
                         be taken in time (ref. AsyncGuard in system.h). Once anything was deferred,
                         every following event is deferred too until loop() catches up, so a later
                         event handled right away never overtakes an earlier deferred one:
                             AsyncGuard guard;
                             if (deferred.defer(guard)) { deferred.run(callback); return; }
                         Handlers that only learn the guard state from their caller use
                         `auto scope = deferred.scope(guard);` around the call and `defer()` inside.
- ConnectionIds          ids of the connections owned by a network task, for the output check below
- registerPendingOutput  restart() waits for the output some module did not send out yet
- TaskQueue<T, Max>      items from other tasks for loop(), at most Max bytes of them

Per chip, ref. arch/esp8266/tasks_esp8266.h and arch/rtos/tasks_rtos.h (ESP32 and LibreTiny)

*/

#pragma once

#include "system.h"

#if defined(ESP8266)
#include "arch/esp8266/tasks_esp8266.h"
#elif defined(ESP32) || defined(LIBRETINY)
#include "arch/rtos/tasks_rtos.h"
#endif
