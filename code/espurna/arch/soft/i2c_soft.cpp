/*

I2C MODULE: the one software I2C master (chip::i2c_* of arch/rtos/chip_sdk.h) of every chip without a hardware port
ESPurna drives: ESP8266 and Beken. Any two pins, on espurna::pins (pins.h) and chip::delay_us() only - nothing of a
chip's SDK. ESP32 has its hardware port instead (arch/esp32/i2c_esp32.cpp; arch/soft/ is not in its CMakeLists.txt).
In the i2c module only (custom_modules of platformio_esp8266.ini / platformio_beken.ini), the head has no I2C

Open drain by hand: a line is driven low (output 0) or released to its pull-up (input with the pull-up), never driven
high. A slave may stretch the clock: SCL is released, then read back until it is high

*/

#include "espurna.h"

#if I2C_SUPPORT

#include <chip_sdk.h>

#include "../../pins.h"

#include <algorithm>
#include <cstdint>

namespace espurna {
namespace core {
namespace chip {
namespace {

struct I2c {
    int sda { -1 };
    int scl { -1 };
    uint32_t half { 5 };    // half a clock period, us (100 kHz)
};

I2c i2c;

// how long a slave may hold SCL low, us
constexpr int StretchMax { 1000 };

void i2c_release(int pin) {
    pins::mode(pin, pins::InputPullup);
}

void i2c_low(int pin) {
    pins::write(pin, false);
    pins::mode(pin, pins::Output);
    pins::write(pin, false);
}

void i2c_wait() {
    delay_us(i2c.half);
}

bool i2c_scl_high() {
    i2c_release(i2c.scl);
    for (int attempt = 0; attempt < StretchMax; ++attempt) {
        if (pins::read(i2c.scl)) {
            i2c_wait();
            return true;
        }
        delay_us(1);
    }

    return false;
}

bool i2c_start() {
    i2c_release(i2c.sda);
    if (!i2c_scl_high() || !pins::read(i2c.sda)) {
        return false;
    }

    i2c_low(i2c.sda);
    i2c_wait();
    i2c_low(i2c.scl);
    i2c_wait();
    return true;
}

void i2c_stop() {
    i2c_low(i2c.sda);
    i2c_wait();
    i2c_scl_high();
    i2c_release(i2c.sda);
    i2c_wait();
}

bool i2c_write_byte(uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
        if (value & (1 << bit)) {
            i2c_release(i2c.sda);
        } else {
            i2c_low(i2c.sda);
        }
        i2c_wait();
        if (!i2c_scl_high()) {
            return false;
        }
        i2c_low(i2c.scl);
    }

    // ACK: the slave pulls SDA low
    i2c_release(i2c.sda);
    i2c_wait();
    if (!i2c_scl_high()) {
        return false;
    }

    const bool ack = !pins::read(i2c.sda);
    i2c_low(i2c.scl);
    i2c_wait();
    return ack;
}

uint8_t i2c_read_byte(bool ack) {
    uint8_t out = 0;
    i2c_release(i2c.sda);
    for (int bit = 7; bit >= 0; --bit) {
        i2c_wait();
        i2c_scl_high();
        if (pins::read(i2c.sda)) {
            out |= (1 << bit);
        }
        i2c_low(i2c.scl);
    }

    if (ack) {
        i2c_low(i2c.sda);
    } else {
        i2c_release(i2c.sda);
    }

    i2c_wait();
    i2c_scl_high();
    i2c_low(i2c.scl);
    i2c_release(i2c.sda);
    i2c_wait();
    return out;
}

} // namespace

bool i2c_begin(uint8_t sda, uint8_t scl, uint32_t frequency) {
    if (!pin_valid(sda) || !pin_valid(scl)) {
        return false;
    }

    i2c.sda = sda;
    i2c.scl = scl;
    i2c_clock(frequency);
    i2c_release(i2c.sda);
    i2c_release(i2c.scl);
    return true;
}

void i2c_end() {
    i2c.sda = -1;
    i2c.scl = -1;
}

void i2c_clock(uint32_t frequency) {
    i2c.half = frequency ? std::max<uint32_t>(1, 500000 / frequency) : 5;
}

// 0 ok, 2 address NACK, 3 data NACK, 4 bus error (Wire's endTransmission() codes). Without the stop the bus stays
// taken: the read that follows starts again (repeated start)
uint8_t i2c_write(uint8_t address, const uint8_t* data, size_t size, bool stop) {
    if ((i2c.sda < 0) || !i2c_start()) {
        return 4;
    }

    uint8_t result = 0;
    if (!i2c_write_byte(static_cast<uint8_t>(address << 1))) {
        result = 2;
    }

    for (size_t index = 0; !result && (index < size); ++index) {
        if (!i2c_write_byte(data[index])) {
            result = 3;
        }
    }

    if (stop || result) {
        i2c_stop();
    }

    return result;
}

size_t i2c_read(uint8_t address, uint8_t* out, size_t size, bool stop) {
    if ((i2c.sda < 0) || !i2c_start() || !i2c_write_byte(static_cast<uint8_t>((address << 1) | 1))) {
        i2c_stop();
        return 0;
    }

    for (size_t index = 0; index < size; ++index) {
        out[index] = i2c_read_byte((index + 1) < size);
    }

    if (stop) {
        i2c_stop();
    }

    return size;
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif // I2C_SUPPORT
