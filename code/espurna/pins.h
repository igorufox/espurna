/*

The pins of the chip for ESPurna's own code (owner 2026-11-18: without the Arduino layer): the GPIO, ADC and PWM calls of
arch/rtos/chip_sdk.h (core_esp8266.cpp, core_esp32.cpp, the Beken core) under one short name. Instead of pinMode(),
digitalWrite(), digitalRead(), analogRead(), analogWrite() and HIGH / LOW / INPUT / OUTPUT of the layer.

    espurna::pins::mode(pin, espurna::pins::Output);
    espurna::pins::write(pin, true);

*/

#pragma once

#include <cstdint>

#include <chip_sdk.h>

// IRAM_ATTR of the chip: .iram1 on ESP8266 and ESP32 (esp_attr.h), nothing on Beken (no IRAM, the module linker has no
// place for an IRAM section there)
#if __has_include(<esp_attr.h>)
#include <esp_attr.h>
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

namespace espurna {
namespace pins {

constexpr uint8_t Input { core::chip::pin::Input };
constexpr uint8_t Output { core::chip::pin::Output };
constexpr uint8_t InputPullup { core::chip::pin::InputPullup };
constexpr uint8_t InputPulldown { core::chip::pin::InputPulldown };
constexpr uint8_t OutputOpenDrain { core::chip::pin::OutputOpenDrain };

// the first ADC pin of the chip (ESP8266: the TOUT pad, ESP32: GPIO36 of ADC1)
#if defined(ESP8266)
constexpr uint8_t Adc0 { 17 };
#elif defined(ESP_PLATFORM)
constexpr uint8_t Adc0 { 36 };
#else
constexpr uint8_t Adc0 { 0 };
#endif

// interrupt edges
constexpr uint8_t Rising { core::chip::edge::Rising };
constexpr uint8_t Falling { core::chip::edge::Falling };
constexpr uint8_t Change { core::chip::edge::Change };

inline void mode(uint8_t pin, uint8_t value) {
    core::chip::pin_mode(pin, value);
}

inline void write(uint8_t pin, bool level) {
    core::chip::pin_write(pin, level);
}

inline bool read(uint8_t pin) {
    return core::chip::pin_read(pin);
}

// raw value, adc_bits() of resolution
inline int analog(uint8_t pin) {
    return core::chip::adc_read(pin);
}

// PWM of `duty` out of `range` at `frequency` Hz; 0 stops it
inline void pwm(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency) {
    core::chip::pwm_write(pin, duty, range, frequency);
}

// an interrupt on an edge: `handler(arg)`, runs in the interrupt (IRAM_ATTR, nothing in flash)
inline bool attach(uint8_t pin, core::chip::PinHandler handler, void* arg, uint8_t edge) {
    return core::chip::pin_attach(pin, handler, arg, edge);
}

// a plain handler(): the trampoline in IRAM passes it as the argument
inline void IRAM_ATTR call_plain(void* handler) {
    reinterpret_cast<void (*)()>(handler)();
}

inline bool attach(uint8_t pin, void (*handler)(), uint8_t edge) {
    return attach(pin, call_plain, reinterpret_cast<void*>(handler), edge);
}

inline void detach(uint8_t pin) {
    core::chip::pin_detach(pin);
}

} // namespace pins
} // namespace espurna
