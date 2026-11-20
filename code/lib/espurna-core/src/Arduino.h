#pragma once
// ESPurna's own Arduino.h: the Arduino API names (String, Print, Stream, IPAddress, millis, pinMode, Serial ...)
// without the Arduino framework. Owner 2026-11-17: Arduino goes from every build, the names stay as our code so
// the sensor libraries build on them unchanged.
//
// arduino-api/ is ArduinoCore-API 2022.08.24 (LGPL-2.1), an unchanged copy (the same version LibreTiny 1.14 uses), so
// an update is a plain copy again. Everything else here is ours.

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
using ::round;
using std::abs;
using std::isinf;
using std::isnan;
using std::max;
using std::min;
#endif

#include "arduino-api/ArduinoAPI.h"

// dtostrf() (String(float), sensors), every core's Arduino.h has it
#include "arduino-api/deprecated-avr-comp/avr/dtostrf.h"

#ifdef __cplusplus
#include "arduino-api/HardwareSerial.h"
#include "arduino-api/RingBuffer.h"
using namespace arduino;
#endif

// the pins of the board (A0, SDA, SCL ...), as every core's Arduino.h has them: arch/<chip>/pins_arduino.h
#if __has_include(<pins_arduino.h>)
#include <pins_arduino.h>
#endif

// what the chip's own core gave the libraries besides the API (ESP32: FreeRTOS, log_x(), timer*, rmt*):
// arch/<chip>/core_arduino_chip.h, on the include path of the chip's ini
#if __has_include(<core_arduino_chip.h>)
#include <core_arduino_chip.h>
#endif

// Names of the ESP8266 / ESP32 cores the libraries call besides the API (wiring.cpp)
// Print::printf_P(): the format string is read where it is (pgmspace.h of the chip), it is printf()
#ifndef printf_P
#define printf_P printf
#endif

// a string in flash as the String / Print argument of the ESP cores (pgmspace.h)
#ifdef __cplusplus
#define FPSTR(pstr_pointer) (reinterpret_cast<const __FlashStringHelper*>(pstr_pointer))
#endif

#define NOT_AN_INTERRUPT -1
#define digitalPinToInterrupt(pin) (static_cast<int>(pin))

#ifdef __cplusplus
extern "C" {
#endif

void noInterrupts(void);
void interrupts(void);
void optimistic_yield(uint32_t interval_us);

#ifdef __cplusplus
} // extern "C"

void analogWriteRange(uint32_t range);
void analogWriteResolution(uint8_t bits);
void analogWriteFreq(uint32_t frequency);
void analogWriteFrequency(uint32_t frequency);

inline void attachInterruptArg(uint8_t pin, voidFuncPtrParam callback, void* arg, int mode) {
    attachInterruptParam(pin, callback, static_cast<PinStatus>(mode), arg);
}

#include "HardwareSerialPort.h"
#include "Esp.h"

// the empty String of the ESP cores (WString.h)
extern const arduino::String emptyString;

// ultoa() of the ESP cores' stdlib_noniso.h (itoa / ltoa / utoa: arduino-api/itoa.h)
extern "C" char* ultoa(unsigned long value, char* out, int radix);
#endif
