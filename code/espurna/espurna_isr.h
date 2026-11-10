/*

GPIO INTERRUPTS

Chip part of the code running in GPIO interrupt handlers, ref. arch/<chip>/isr_<chip>.h

- Pins                   size of a table indexed by the pin number
- attachable(pin)        pin can have an interrupt handler
- attach(pin, ...)       handler with an argument, CHANGE / RISING / FALLING
- read(pin)              pin level, safe to call from the handler (no flash access)
- Lock                   state shared between the handler and the task, one per owner of that state
- Lock::Guard            task side, the handler does not run while it is held
- Lock::IsrGuard         handler side, only blocks anything when tasks run on another core
- Lock::sync(lock)       after detachInterrupt(), handler running on another core is done

*/

#pragma once

#include <Arduino.h>

#if defined(ESP8266)
#include "arch/esp8266/isr_esp8266.h"
#elif defined(ESP32)
#include "arch/esp32/isr_esp32.h"
#elif defined(LIBRETINY)
#include "arch/beken/isr_beken.h"
#endif
