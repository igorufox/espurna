/*

Part of SETTINGS MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2023 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once


#include <cstdint>
#include <type_traits>

#include "settings_helpers.h"
#include "text_types.h"

namespace espurna {
namespace settings {
namespace internal {

// uint32_t / int32_t are unsigned int / int with the xtensa-lx106 (esp8266) toolchain, unsigned long / long with
// the ESP-IDF 5 and arm-none-eabi (LibreTiny) ones. The other type of each pair gets its own overload
using OtherUnsigned = std::conditional<std::is_same<uint32_t, unsigned int>::value, unsigned long, unsigned int>::type;
using OtherSigned = std::conditional<std::is_same<int32_t, int>::value, long, int>::type;

template <typename T>
T convert(const espurna::string& value);

template <>
float convert(const espurna::string& value);

template <>
double convert(const espurna::string& value);

template <>
signed char convert(const espurna::string& value);

template <>
short convert(const espurna::string& value);

template <>
int convert(const espurna::string& value);

template <>
long convert(const espurna::string& value);

template <>
bool convert(const espurna::string& value);

template <>
unsigned long convert(const espurna::string& value);

template <>
unsigned int convert(const espurna::string& value);

template <>
unsigned short convert(const espurna::string& value);

template <>
unsigned char convert(const espurna::string& value);

template <>
duration::Microseconds convert(const espurna::string&);

template <>
duration::Milliseconds convert(const espurna::string&);

template <>
duration::Seconds convert(const espurna::string&);

template <>
duration::Minutes convert(const espurna::string&);

template <>
duration::Hours convert(const espurna::string&);

inline espurna::string serialize(uint8_t value, int base = 10) {
    return espurna::string(value, base);
}

inline espurna::string serialize(uint16_t value, int base = 10) {
    return espurna::string(value, base);
}

espurna::string serialize(uint32_t value, int base = 10);

inline espurna::string serialize(OtherUnsigned value, int base = 10) {
    return serialize(static_cast<uint32_t>(value), base);
}

inline espurna::string serialize(OtherSigned value, int base = 10) {
    return espurna::string(value, base);
}

inline espurna::string serialize(int16_t value, int base = 10) {
    return espurna::string(value, base);
}

inline espurna::string serialize(int32_t value, int base = 10) {
    return espurna::string(value, base);
}

inline espurna::string serialize(int8_t value, int base = 10) {
    return serialize(static_cast<int32_t>(value), base);
}

inline espurna::string serialize(bool value) {
    return value ? PSTR("true") : PSTR("false");
}

inline espurna::string serialize(double value) {
    char buffer[24];
    formatDecimal(value, 3, buffer, sizeof(buffer));
    return espurna::string(buffer);
}

inline espurna::string serialize(float value) {
    return serialize(static_cast<double>(value));
}

espurna::string serialize(duration::Microseconds);

espurna::string serialize(duration::Milliseconds);

espurna::string serialize(duration::Seconds);

espurna::string serialize(duration::Minutes);

espurna::string serialize(duration::Hours);

template <typename Container, typename T>
T convert(const Container& options, const espurna::string& value, T defaultValue) {
    if (value.length()) {
        using espurna::settings::options::Enumeration;
        using UnderlyingType = typename Enumeration<T>::UnderlyingType;
        typename Enumeration<T>::Numeric numeric;
        numeric.check(value, convert<UnderlyingType>);

        for (auto it = std::begin(options); it != std::end(options); ++it) {
            if (numeric && ((*it).numeric() == numeric.value())) {
                return static_cast<T>(numeric.value());
            } else if (!numeric && ((*it) == value)) {
                return (*it).value();
            }
        }
    }

    return defaultValue;
}

template <typename Container, typename T>
espurna::string serialize(const Container& options, T value) {
    espurna::string out;

    for (auto it = std::begin(options); it != std::end(options); ++it) {
        if ((*it).value() == value) {
            out = (*it).string().toString();
            break;
        }
    }

    return out;
}

} // namespace internal
} // namespace settings
} // namespace espurna

