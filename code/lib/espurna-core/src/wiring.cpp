// The Arduino wiring functions, once for every chip, over core_chip.h

#include <Arduino.h>

#include "core_chip.h"

namespace chip = espurna::core::chip;

const arduino::String emptyString;

namespace {

struct Pwm {
    uint32_t range { 255 };
    uint32_t frequency { 1000 };
};

Pwm pwm;

// the PWM is the light_pwm module's (chip_sdk.h PwmBus): nothing without it
void pwm_out(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency) {
    if (chip::pwm_bus) {
        chip::pwm_bus->write(pin, duty, range, frequency);
    }
}

uint32_t random_state { 0x2545f491 };

} // namespace

extern "C" {

void yield(void) {
    chip::yield();
}

// the ESP cores' yield() of the long loops: every call here (the scheduler decides)
void optimistic_yield(uint32_t) {
    chip::yield();
}

unsigned long millis(void) {
    return static_cast<unsigned long>(chip::micros() / 1000ULL);
}

unsigned long micros(void) {
    return static_cast<unsigned long>(chip::micros());
}

void delay(unsigned long ms) {
    chip::delay(ms);
}

void delayMicroseconds(unsigned int us) {
    chip::delay_us(us);
}

void pinMode(pin_size_t pin, PinMode mode) {
    chip::pin_mode(pin, mode);
}

void digitalWrite(pin_size_t pin, PinStatus status) {
    chip::pin_write(pin, status != LOW);
}

PinStatus digitalRead(pin_size_t pin) {
    return chip::pin_read(pin) ? HIGH : LOW;
}

int analogRead(pin_size_t pin) {
    return chip::adc_read(pin);
}

void analogReference(uint8_t) {
}

void analogWrite(pin_size_t pin, int value) {
    const auto duty = (value < 0) ? 0u : static_cast<uint32_t>(value);
    pwm_out(pin, (duty > pwm.range) ? pwm.range : duty, pwm.range, pwm.frequency);
}

void attachInterruptParam(pin_size_t pin, voidFuncPtrParam callback, PinStatus mode, void* param) {
    chip::pin_attach(pin, callback, param, mode);
}

static void call_plain(void* arg) {
    reinterpret_cast<voidFuncPtr>(arg)();
}

void attachInterrupt(pin_size_t pin, voidFuncPtr callback, PinStatus mode) {
    chip::pin_attach(pin, call_plain, reinterpret_cast<void*>(callback), mode);
}

void detachInterrupt(pin_size_t pin) {
    chip::pin_detach(pin);
}

void shiftOut(pin_size_t data, pin_size_t clock, BitOrder order, uint8_t value) {
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = (order == LSBFIRST) ? i : (7 - i);
        chip::pin_write(data, (value >> bit) & 1);
        chip::pin_write(clock, true);
        chip::pin_write(clock, false);
    }
}

uint8_t shiftIn(pin_size_t data, pin_size_t clock, BitOrder order) {
    uint8_t out = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        chip::pin_write(clock, true);
        const uint8_t bit = (order == LSBFIRST) ? i : (7 - i);
        out |= static_cast<uint8_t>(chip::pin_read(data)) << bit;
        chip::pin_write(clock, false);
    }
    return out;
}

} // extern "C"

// Pulse length in us, 0 on timeout (no interrupts, the HLW8012 without them and NewPing)
static unsigned long pulse(uint8_t pin, uint8_t state, unsigned long timeout) {
    const bool level = state != LOW;
    const auto start = chip::micros();
    const auto expired = [&]() {
        return (chip::micros() - start) >= timeout;
    };

    while (chip::pin_read(pin) == level) {
        if (expired()) {
            return 0;
        }
    }

    while (chip::pin_read(pin) != level) {
        if (expired()) {
            return 0;
        }
    }

    const auto begin = chip::micros();
    while (chip::pin_read(pin) == level) {
        if (expired()) {
            return 0;
        }
    }

    return static_cast<unsigned long>(chip::micros() - begin);
}

// arduino-api/Common.h declares them in extern "C" and again in C++ with the default timeout: one function
extern "C" unsigned long pulseIn(pin_size_t pin, uint8_t state, unsigned long timeout) {
    return pulse(pin, state, timeout);
}

extern "C" unsigned long pulseInLong(pin_size_t pin, uint8_t state, unsigned long timeout) {
    return pulse(pin, state, timeout);
}

extern "C" void noInterrupts(void) {
    chip::irq_disable();
}

extern "C" void interrupts(void) {
    chip::irq_enable();
}

void tone(uint8_t pin, unsigned int frequency, unsigned long duration) {
    // a square wave; the duration is the caller's (no timer here), noTone() stops it
    (void)duration;
    pwm_out(pin, 128, 255, frequency);
}

void noTone(uint8_t pin) {
    pwm_out(pin, 0, 255, 1000);
}

// xorshift32: random() of the libraries (rc-switch, RFM69 ...); keys and TLS use the hardware RNG of the chip
long random(long max) {
    if (max <= 0) {
        return 0;
    }

    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return static_cast<long>(random_state % static_cast<uint32_t>(max));
}

long random(long min, long max) {
    if (min >= max) {
        return min;
    }

    return random(max - min) + min;
}

void randomSeed(unsigned long seed) {
    if (seed != 0) {
        random_state = static_cast<uint32_t>(seed);
    }
}

// The PWM range / frequency of analogWrite(), names of the ESP8266 and ESP32 cores
void analogWriteRange(uint32_t range) {
    pwm.range = range ? range : 1;
}

void analogWriteResolution(uint8_t bits) {
    pwm.range = (bits >= 32) ? UINT32_MAX : ((1UL << bits) - 1);
}

void analogWriteFreq(uint32_t frequency) {
    pwm.frequency = frequency;
}

void analogWriteFrequency(uint32_t frequency) {
    pwm.frequency = frequency;
}
