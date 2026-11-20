/*

ESP32 extras of the layer (core_esp32.cpp) for ESPurna's own drivers: LEDC channels by number (pwm_chip.h,
buzzer_chip.h), the calibrated ADC (adc_chip.h), the loop task watchdog (system_esp32.cpp)

*/

#pragma once

#include <cstdint>

namespace espurna {
namespace core {
namespace esp32 {

// LEDC channel 0..15 (0..7 high speed, 8..15 low speed, two channels per timer) on the pin
bool ledc_attach(uint8_t pin, uint8_t channel, uint32_t frequency, uint8_t bits);
void ledc_write(uint8_t channel, uint32_t duty);

// millivolts of the pin, calibrated with the eFuse Vref (11 dB attenuation, 0..3100 mV)
uint32_t adc_millivolts(uint8_t pin);

} // namespace esp32
} // namespace core
} // namespace espurna

// the task watchdog of the loop task (Arduino-ESP32's name), fed before every loop() pass
void enableLoopWDT();
extern bool loopTaskWDTEnabled;
