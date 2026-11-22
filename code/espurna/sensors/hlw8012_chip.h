/*

HLW8012 / BL0937 / HJL-01: the pulse widths of CF (active power) and CF1 (current or voltage, chosen by SEL) to the
values, on espurna::pins and espurna::time (no library, no Arduino emulation). The logic of the HLW8012 library of
Xose Pérez (GPL 3, Copyright (C) 2016-2023), as the sensor used it: interrupts on CF and CF1, or pulse_in() without
them.

*/

#pragma once

#include "../pins.h"
#include "../system_time.h"

#include <cmath>
#include <cstdint>

#ifndef V_REF
#define V_REF               2.43
#endif
#ifndef F_OSC
#define F_OSC               3579000
#endif
#ifndef PULSE_TIMEOUT
#define PULSE_TIMEOUT       2000000
#endif

class Hlw8012Chip {
public:
    enum class Mode {
        Current,
        Voltage,
    };

    void begin(uint8_t cf_pin, uint8_t cf1_pin, uint8_t sel_pin, uint8_t current_when,
            bool use_interrupts, uint32_t pulse_timeout) {
        _cf_pin = cf_pin;
        _cf1_pin = cf1_pin;
        _sel_pin = sel_pin;
        _current_mode = current_when ? 1 : 0;
        _use_interrupts = use_interrupts;
        _pulse_timeout = pulse_timeout;

        espurna::pins::mode(_cf_pin, espurna::pins::InputPullup);
        espurna::pins::mode(_cf1_pin, espurna::pins::InputPullup);
        espurna::pins::mode(_sel_pin, espurna::pins::Output);

        default_multipliers();
        _mode = _current_mode;
        espurna::pins::write(_sel_pin, _mode);
    }

    void setMode(Mode mode) {
        _mode = (mode == Mode::Current) ? _current_mode : (1 - _current_mode);
        espurna::pins::write(_sel_pin, _mode);
        if (_use_interrupts) {
            _last_cf1_interrupt = _first_cf1_interrupt = espurna::time::us();
        }
    }

    Mode getMode() const {
        return (_mode == _current_mode) ? Mode::Current : Mode::Voltage;
    }

    Mode toggleMode() {
        const auto mode = (getMode() == Mode::Current) ? Mode::Voltage : Mode::Current;
        setMode(mode);
        return mode;
    }

    double getCurrent() {
        if (_power == 0) {
            _current_pulse_width = 0;
        } else if (_use_interrupts) {
            check_cf1();
        } else if (_mode == _current_mode) {
            _current_pulse_width = pulse_in(_cf1_pin, _pulse_timeout);
        }

        _current = (_current_pulse_width > 0) ? _current_multiplier / _current_pulse_width / 2 : 0;
        return _current;
    }

    unsigned int getVoltage() {
        if (_use_interrupts) {
            check_cf1();
        } else if (_mode != _current_mode) {
            _voltage_pulse_width = pulse_in(_cf1_pin, _pulse_timeout);
        }

        _voltage = (_voltage_pulse_width > 0) ? _voltage_multiplier / _voltage_pulse_width / 2 : 0;
        return _voltage;
    }

    unsigned int getActivePower() {
        if (_use_interrupts) {
            check_cf();
        } else {
            _power_pulse_width = pulse_in(_cf_pin, _pulse_timeout);
        }

        _power = (_power_pulse_width > 0) ? _power_multiplier / _power_pulse_width / 2 : 0;
        return _power;
    }

    unsigned int getApparentPower() {
        const double current = getCurrent();
        const unsigned int voltage = getVoltage();
        return voltage * current;
    }

    double getPowerFactor() {
        const unsigned int active = getActivePower();
        const unsigned int apparent = getApparentPower();
        if (active > apparent) {
            return 1;
        }

        if (apparent == 0) {
            return 0;
        }

        return static_cast<double>(active) / apparent;
    }

    // Ws: the pulses of CF since the last reset, E = P * t = m * N
    unsigned long getEnergy() const {
        if (!_use_interrupts) {
            return 0;
        }

        return _pulse_count * _power_multiplier / 1000000. / 2;
    }

    void resetEnergy() {
        _pulse_count = 0;
    }

    void setCurrentMultiplier(double value) {
        _current_multiplier = value;
    }

    void setVoltageMultiplier(double value) {
        _voltage_multiplier = value;
    }

    void setPowerMultiplier(double value) {
        _power_multiplier = value;
    }

    void IRAM_ATTR cf_interrupt() {
        const uint32_t now = espurna::time::us();
        _power_pulse_width = now - _last_cf_interrupt;
        _last_cf_interrupt = now;
        ++_pulse_count;
    }

    void IRAM_ATTR cf1_interrupt() {
        const uint32_t now = espurna::time::us();
        if ((now - _first_cf1_interrupt) > _pulse_timeout) {
            const uint32_t width = (_last_cf1_interrupt == _first_cf1_interrupt)
                ? 0
                : now - _last_cf1_interrupt;

            if (_mode == _current_mode) {
                _current_pulse_width = width;
            } else {
                _voltage_pulse_width = width;
            }

            _mode = 1 - _mode;
            espurna::pins::write(_sel_pin, _mode);
            _first_cf1_interrupt = now;
        }

        _last_cf1_interrupt = now;
    }

private:
    // the width of a high pulse of the pin in us, 0 when none came in `timeout` us
    static uint32_t pulse_in(uint8_t pin, uint32_t timeout) {
        const uint32_t start = espurna::time::us();
        const auto waited = [&]() {
            return (espurna::time::us() - start) > timeout;
        };

        while (espurna::pins::read(pin)) {
            if (waited()) {
                return 0;
            }
        }

        while (!espurna::pins::read(pin)) {
            if (waited()) {
                return 0;
            }
        }

        const uint32_t rise = espurna::time::us();
        while (espurna::pins::read(pin)) {
            if (waited()) {
                return 0;
            }
        }

        return espurna::time::us() - rise;
    }

    void check_cf() {
        if ((espurna::time::us() - _last_cf_interrupt) > _pulse_timeout) {
            _power_pulse_width = 0;
        }
    }

    void check_cf1() {
        if ((espurna::time::us() - _last_cf1_interrupt) > _pulse_timeout) {
            if (_mode == _current_mode) {
                _current_pulse_width = 0;
            } else {
                _voltage_pulse_width = 0;
            }

            toggleMode();
        }
    }

    void default_multipliers() {
        _current_multiplier = (1000000.0 * 512 * V_REF / _current_resistor / 24.0 / F_OSC);
        _voltage_multiplier = (1000000.0 * 512 * V_REF * _voltage_resistor / 2.0 / F_OSC);
        _power_multiplier = (1000000.0 * 128 * V_REF * V_REF * _voltage_resistor / _current_resistor / 48.0 / F_OSC);
    }

    uint8_t _cf_pin { 0 };
    uint8_t _cf1_pin { 0 };
    uint8_t _sel_pin { 0 };

    double _current_resistor { 0.001 };
    double _voltage_resistor { 2821 };

    double _current_multiplier { 0 };
    double _voltage_multiplier { 0 };
    double _power_multiplier { 0 };

    uint32_t _pulse_timeout { PULSE_TIMEOUT };
    volatile uint32_t _voltage_pulse_width { 0 };
    volatile uint32_t _current_pulse_width { 0 };
    volatile uint32_t _power_pulse_width { 0 };
    volatile uint32_t _pulse_count { 0 };

    double _current { 0 };
    unsigned int _voltage { 0 };
    unsigned int _power { 0 };

    uint8_t _current_mode { 1 };
    volatile uint8_t _mode { 1 };
    bool _use_interrupts { true };

    volatile uint32_t _last_cf_interrupt { 0 };
    volatile uint32_t _last_cf1_interrupt { 0 };
    volatile uint32_t _first_cf1_interrupt { 0 };
};
