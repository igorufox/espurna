// -----------------------------------------------------------------------------
// Analog Sensor
// Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
//
// Scaling support by Carlos Iván Conde Martín <ivan dot conde at gmail dot com>
// (original sensor was just the analogRead output)
// -----------------------------------------------------------------------------

#pragma once

#include <algorithm>

#include "../espurna.h"
#include "../sensor.h"
#include "../adc_driver.h"

#include "BaseSensor.h"
#include "BaseAnalogSensor.h"

class AnalogSensor : public BaseAnalogSensor {
    public:
        static constexpr int RawBits { espurna::adc::chip::bits() };

        static constexpr double RawMin { 0.0 };
        static constexpr double RawMax { (1 << RawBits) - 1 };

        static constexpr size_t SamplesMin { 1 };
        static constexpr size_t SamplesMax { 16 };

        using Microseconds = espurna::duration::critical::Microseconds;

        static constexpr auto DelayMin = Microseconds{ 200 };
        static constexpr auto DelayMax = Microseconds::max();

        unsigned char id() const override {
            return SENSOR_ANALOG_ID;
        }

        unsigned char count() const override {
            return 1;
        }

        void setDelay(Microseconds delay) {
            _delay = std::clamp(delay, DelayMin, DelayMax);
        }

        void setDelay(uint16_t delay) {
            setDelay(Microseconds{delay});
        }

        void setSamples(size_t samples) {
            _samples = std::clamp(samples, SamplesMin, SamplesMax);
        }

        void setFactor(double factor) {
            _factor = factor;
        }

        void setOffset(double offset) {
            _offset = offset;
        }

        void setPin(uint8_t pin) {
            _pin = pin;
        }

        // ---------------------------------------------------------------------

        size_t getSamples() const {
            return _samples;
        }

        Microseconds getDelay() const {
            return _delay;
        }

        double getFactor() const {
            return _factor;
        }

        double getOffset() const {
            return _offset;
        }

        // ---------------------------------------------------------------------
        // Sensor API
        // ---------------------------------------------------------------------

        // Initialization method, must be idempotent
        void begin() override {
            espurna::adc::chip::attenuation(_pin);
            _ready = true;
            _last = TimeSource::now() + _delay;
        }

        // Descriptive name of the sensor
        String description() const override {
            return String(F("ANALOG @ ")) + espurna::adc::chip::pad(_pin);
        }

        // Address of the sensor (it could be the GPIO or I2C address)
        String address(unsigned char) const override {
            return espurna::adc::chip::address(_pin);
        }

        // Type for slot # index
        unsigned char type(unsigned char index) const override {
            if (index == 0) {
                return MAGNITUDE_ANALOG;
            }

            return MAGNITUDE_NONE;
        }

        // Current value for slot # index
        double value(unsigned char index) override {
            if (index == 0) {
                return _sampledValue();
            }

            return 0;
        }

        void tick() override {
            _readNext(_pin);
        }

    protected:
        // Voltage at the pin, ref. adc_driver.h (ESP8266 ADC range is 0..1V, ESP32 one is not linear and is read in calibrated mV)
        double _sampledVoltageValue() const {
            return _millivolts / 1000.0;
        }

        double _sampledValue() const {
            return _value;
        }

        void _sampledValue(double value) {
            _value = value;
        }

        void _readNext(uint8_t pin) {
            if (_sample >= _samples) {
                return;
            }

            const auto now = TimeSource::now();
            if (now - _last < _delay) {
                return;
            }

            ++_sample;
            _last = now;
            const auto raw = ::analogRead(pin);
            _sum += raw;
            _sum_millivolts += espurna::adc::chip::millivolts(pin, raw);

            if (_sample >= _samples) {
                const double sum = _sum;
                const double samples = _samples;
                _sampledValue(sum / samples);
                _sum = 0;
                _millivolts = _sum_millivolts / samples;
                _sum_millivolts = 0;
                _sample = 0;
            }
        }

        double _withFactor(double value) const {
            return _factor * value + _offset;
        }

        double _minWithFactor() const {
            return _withFactor(RawMin);
        }

        double _maxWithFactor() const {
            return _withFactor(RawMax);
        }

        using TimeSource = espurna::time::CpuClock;
        TimeSource::time_point _last{};
        Microseconds _delay { DelayMin };

        size_t _samples { SamplesMin };
        size_t _sample { 0 };

        uint32_t _sum { 0 };

        double _value { 0.0 };

        double _sum_millivolts { 0.0 };
        double _millivolts { 0.0 };

        double _factor { 1.0 };
        double _offset { 0.0 };

        uint8_t _pin { A0 };
};

#ifndef __cpp_inline_variables
constexpr int AnalogSensor::RawBits;

constexpr double AnalogSensor::RawMin;
constexpr double AnalogSensor::RawMax;

constexpr size_t AnalogSensor::SamplesMin;
constexpr size_t AnalogSensor::SamplesMax;

constexpr AnalogSensor::Microseconds AnalogSensor::DelayMin;
constexpr AnalogSensor::Microseconds AnalogSensor::DelayMax;
#endif

