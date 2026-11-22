/*

1-Wire master on one pin (the dallas driver, driver_onewire.cpp), on espurna::pins and the chip's interrupt lock (no
library, no Arduino emulation). The timings, the ROM search and the CRCs of the OneWire library (Jim Studt, Paul
Stoffregen and the contributors, MIT-style licence): reset 480 us low and the presence at 70 us, slots of 65 us.

The line: an open-drain output that reads back its level (ESP32, ESP8266: set once, a slot only writes and reads), or
input / output switched at every slot where the chip has no such mode (Beken: its open drain is high impedance only).
The pull-up is on the line (4.7 kOhm).

*/

#pragma once

#include "pins.h"
#include "system_time.h"

#include <chip_sdk.h>

#include <cstdint>
#include <cstring>

class OneWireBus {
public:
    explicit OneWireBus(uint8_t pin) :
        _pin(pin)
    {
        line();
    }

    // true - a device answered with its presence pulse
    bool reset() {
        line();

        // the line must be high (no device holds it low) before the reset pulse
        int retries = 125;
        while (!level()) {
            if (--retries == 0) {
                return false;
            }
            espurna::time::sleep_us(2);
        }

        low();
        espurna::time::sleep_us(480);

        lock();
        release();
        espurna::time::sleep_us(70);
        const bool presence = !level();
        unlock();

        espurna::time::sleep_us(410);
        return presence;
    }

    void write(uint8_t value, bool power = false) {
        for (uint8_t mask = 0x01; mask; mask <<= 1) {
            write_bit((value & mask) != 0);
        }

        if (!power) {
            release();
        }
    }

    void write_bytes(const uint8_t* data, size_t size, bool power = false) {
        for (size_t index = 0; index < size; ++index) {
            write(data[index], true);
        }

        if (!power) {
            release();
        }
    }

    uint8_t read() {
        uint8_t out = 0;
        for (uint8_t mask = 0x01; mask; mask <<= 1) {
            if (read_bit()) {
                out |= mask;
            }
        }

        return out;
    }

    void read_bytes(uint8_t* out, size_t size) {
        for (size_t index = 0; index < size; ++index) {
            out[index] = read();
        }
    }

    void select(const uint8_t* rom) {
        write(0x55);
        for (int index = 0; index < 8; ++index) {
            write(rom[index]);
        }
    }

    void skip() {
        write(0xcc);
    }

    void reset_search() {
        _last_discrepancy = 0;
        _last_device = false;
        std::memset(_rom, 0, sizeof(_rom));
    }

    // the next ROM on the bus into `out` (8 bytes), false - no more
    bool search(uint8_t* out) {
        uint8_t id_bit_number = 1;
        uint8_t last_zero = 0;
        uint8_t rom_byte_number = 0;
        uint8_t rom_byte_mask = 1;
        bool result = false;

        if (!_last_device) {
            if (!reset()) {
                reset_search();
                return false;
            }

            write(0xf0);

            do {
                const bool id_bit = read_bit();
                const bool cmp_id_bit = read_bit();
                if (id_bit && cmp_id_bit) {
                    break;
                }

                bool direction;
                if (id_bit != cmp_id_bit) {
                    direction = id_bit;
                } else {
                    direction = (id_bit_number < _last_discrepancy)
                        ? ((_rom[rom_byte_number] & rom_byte_mask) != 0)
                        : (id_bit_number == _last_discrepancy);
                    if (!direction) {
                        last_zero = id_bit_number;
                    }
                }

                if (direction) {
                    _rom[rom_byte_number] |= rom_byte_mask;
                } else {
                    _rom[rom_byte_number] &= static_cast<uint8_t>(~rom_byte_mask);
                }

                write_bit(direction);
                ++id_bit_number;
                rom_byte_mask <<= 1;
                if (!rom_byte_mask) {
                    ++rom_byte_number;
                    rom_byte_mask = 1;
                }
            } while (rom_byte_number < 8);

            if (id_bit_number >= 65) {
                _last_discrepancy = last_zero;
                if (!_last_discrepancy) {
                    _last_device = true;
                }
                result = true;
            }
        }

        if (!result || !_rom[0]) {
            reset_search();
            return false;
        }

        std::memcpy(out, _rom, sizeof(_rom));
        return true;
    }

    // Dallas / Maxim CRC-8 (x^8 + x^5 + x^4 + 1, LSB first)
    static uint8_t crc8(const uint8_t* data, size_t size) {
        uint8_t crc = 0;
        while (size--) {
            uint8_t value = *data++;
            for (int bit = 0; bit < 8; ++bit) {
                const bool mix = (crc ^ value) & 0x01;
                crc >>= 1;
                if (mix) {
                    crc ^= 0x8c;
                }
                value >>= 1;
            }
        }

        return crc;
    }

    // CRC-16 of the DS2406 / DS28E... (x^16 + x^15 + x^2 + 1), not inverted
    static uint16_t crc16(const uint8_t* data, size_t size, uint16_t crc = 0) {
        static constexpr uint8_t OddParity[16] { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };
        for (size_t index = 0; index < size; ++index) {
            uint16_t value = (data[index] ^ crc) & 0xff;
            crc >>= 8;
            if (OddParity[value & 0x0f] ^ OddParity[value >> 4]) {
                crc ^= 0xc001;
            }
            value <<= 6;
            crc ^= value;
            value <<= 1;
            crc ^= value;
        }

        return crc;
    }

private:
#if defined(BK72XX)
    static constexpr bool OpenDrain { false };
#else
    static constexpr bool OpenDrain { true };
#endif

    static void lock() {
        espurna::core::chip::irq_disable();
    }

    static void unlock() {
        espurna::core::chip::irq_enable();
    }

    // the mode of the line before a reset: someone else (the pull-up retry of the search) may have changed it
    void line() {
        if (OpenDrain) {
            espurna::pins::mode(_pin, espurna::pins::OutputOpenDrain);
            espurna::pins::write(_pin, true);
        } else {
            espurna::pins::mode(_pin, espurna::pins::Input);
        }
    }

    void low() {
        if (OpenDrain) {
            espurna::pins::write(_pin, false);
        } else {
            espurna::pins::write(_pin, false);
            espurna::pins::mode(_pin, espurna::pins::Output);
            espurna::pins::write(_pin, false);
        }
    }

    void release() {
        if (OpenDrain) {
            espurna::pins::write(_pin, true);
        } else {
            espurna::pins::mode(_pin, espurna::pins::Input);
        }
    }

    bool level() const {
        return espurna::pins::read(_pin);
    }

    void write_bit(bool value) {
        lock();
        low();
        espurna::time::sleep_us(value ? 10 : 65);
        release();
        unlock();
        espurna::time::sleep_us(value ? 55 : 5);
    }

    bool read_bit() {
        lock();
        low();
        espurna::time::sleep_us(3);
        release();
        espurna::time::sleep_us(10);
        const bool out = level();
        unlock();
        espurna::time::sleep_us(53);
        return out;
    }

    uint8_t _pin;
    uint8_t _rom[8] {};
    uint8_t _last_discrepancy { 0 };
    bool _last_device { false };
};
