// -----------------------------------------------------------------------------
// HC-SR04, SRF05, SRF06, DYP-ME007, JSN-SR04T & Parallax PING)))™
// Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>
// Enhancements by Rui Marinho
// -----------------------------------------------------------------------------

#if SENSOR_SUPPORT && SONAR_SUPPORT

#pragma once

#include "../pins.h"

#include <algorithm>
#include <vector>

#include "BaseSensor.h"

// the ping of the sensor on espurna::pins (no library, no Arduino emulation), as NewPing 1.9 does it: a 12 us pulse on
// the trigger, then the width of the echo (57 us a centimetre there and back), one pin when trigger == echo
namespace espurna {
namespace sensor {
namespace sonar {

static constexpr uint32_t RoundtripCm { 57 };
static constexpr uint32_t TriggerWidth { 12 };
static constexpr uint32_t MaxStartDelay { 5800 };
static constexpr uint32_t MaxDistanceCm { 500 };
static constexpr uint32_t MedianDelayMs { 30 };

// us of the echo, 0 - none within the distance
inline uint32_t ping(uint8_t trigger, uint8_t echo, uint32_t max_cm) {
    const uint32_t max_echo = std::min(max_cm ? max_cm : MaxDistanceCm, MaxDistanceCm) * RoundtripCm
        + (RoundtripCm / 2);

    espurna::pins::mode(trigger, espurna::pins::Output);
    espurna::pins::write(trigger, true);
    espurna::time::sleep_us(TriggerWidth);
    espurna::pins::write(trigger, false);
    if (trigger == echo) {
        espurna::pins::mode(echo, espurna::pins::Input);
    }

    // the previous ping has not ended
    if (espurna::pins::read(echo)) {
        return 0;
    }

    const uint32_t triggered = espurna::time::us();
    while (!espurna::pins::read(echo)) {
        if ((espurna::time::us() - triggered) > (MaxStartDelay + max_echo)) {
            return 0;
        }
    }

    const uint32_t start = espurna::time::us();
    while (espurna::pins::read(echo)) {
        if ((espurna::time::us() - start) > max_echo) {
            return 0;
        }
    }

    return espurna::time::us() - start;
}

// the median of `iterations` pings that had an echo, 30 ms apart
inline uint32_t median(uint8_t trigger, uint8_t echo, uint32_t max_cm, unsigned int iterations) {
    std::vector<uint32_t> out;
    out.reserve(iterations);
    for (unsigned int index = 0; index < iterations; ++index) {
        const auto value = ping(trigger, echo, max_cm);
        if (value) {
            out.push_back(value);
        }

        if (index + 1 < iterations) {
            espurna::time::blockingDelay(espurna::duration::Milliseconds(MedianDelayMs));
        }
    }

    if (out.empty()) {
        return 0;
    }

    std::sort(out.begin(), out.end());
    return out[out.size() / 2];
}

} // namespace sonar
} // namespace sensor
} // namespace espurna

class SonarSensor : public BaseSensor {

    public:

        // ---------------------------------------------------------------------

        // Echo pin.
        void setEcho(unsigned char echo) {
            _echo = echo;
        }

        // Number of iterations to ping in order to filter out erroneous readings
        // using a digital filter.
        void setIterations(unsigned int iterations) {
            _iterations = iterations;
        }

        // Max sensor distance in centimeters.
        void setMaxDistance(unsigned int distance) {
            _max_distance = distance;
        }

        // Trigger pin.
        void setTrigger(unsigned char trigger) {
            _trigger = trigger;
        }

        // ---------------------------------------------------------------------

        unsigned char getEcho() const {
            return _echo;
        }

        unsigned char getTrigger() const {
            return _trigger;
        }

        unsigned int getMaxDistance() const {
            return _max_distance;
        }

        unsigned int getIterations() const {
            return _iterations;
        }

        // ---------------------------------------------------------------------
        // Sensor API
        // ---------------------------------------------------------------------

        unsigned char id() const override {
            return SENSOR_SONAR_ID;
        }

        unsigned char count() const override {
            return 1;
        }

        // Initialization method, must be idempotent
        void begin() override {
            espurna::pins::mode(getEcho(), espurna::pins::Input);
            espurna::pins::mode(getTrigger(), espurna::pins::Output);
            espurna::pins::write(getTrigger(), false);
            _distance = 0.0;
            _ready = true;
        }

        // Descriptive name of the sensor
        espurna::string description() const override {
            char buffer[32];
            snprintf_P(buffer, sizeof(buffer),
                PSTR("Sonar @ GPIO(%hhu, %hhu)"), _trigger, _echo);
            return espurna::string(buffer);
        }

        // Address of the sensor (it could be the GPIO or I2C address)
        espurna::string address(unsigned char) const override {
            return espurna::string(_trigger);
        }

        // Type for slot # index
        unsigned char type(unsigned char index) const override {
            if (index == 0) return MAGNITUDE_DISTANCE;
            return MAGNITUDE_NONE;
        }

        // TODO: fix meter <-> cm conversions
        void pre() override {
            namespace sonar = espurna::sensor::sonar;
            const auto echo = getIterations()
                ? sonar::median(getTrigger(), getEcho(), getMaxDistance(), getIterations())
                : sonar::ping(getTrigger(), getEcho(), getMaxDistance());
            _distance = static_cast<double>(echo / sonar::RoundtripCm) / 100.0;
        }

        // Current value for slot # index
        double value(unsigned char index) override {
            if (index == 0) return _distance;
            return 0.0;
        }

    protected:

        // ---------------------------------------------------------------------
        // Protected
        // ---------------------------------------------------------------------

        unsigned char _trigger;
        unsigned char _echo;

        unsigned int _max_distance;
        unsigned int _iterations;
        double _distance;

};

#endif // SENSOR_SUPPORT && SONAR_SUPPORT
