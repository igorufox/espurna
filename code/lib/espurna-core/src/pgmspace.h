#pragma once
// PROGMEM, PSTR, F(), pgm_read_* for our code and the libraries.
// ESP32 and Beken read flash byte by byte through the cache: the ArduinoCore-API macros (plain memory access).
#pragma once

#include "arduino-api/deprecated-avr-comp/avr/pgmspace.h"

// what the ESP cores' pgmspace.h has besides the AVR names
#include <stdarg.h>
#include <stdio.h>

#ifndef vsnprintf_P
#define vsnprintf_P vsnprintf
#endif

#ifndef vsprintf_P
#define vsprintf_P vsprintf
#endif

#ifndef memmove_P
#define memmove_P memmove
#endif
