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

// arch/<chip>/isr_chip.h: the chip's folder (-I of its ini)
#include "isr_chip.h"
