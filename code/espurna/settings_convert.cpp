/*

Part of SETTINGS MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2023 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/


#include "utils.h"
#include "text_types.h"

#include "settings_convert.h"
#include "settings_helpers.h"

namespace espurna {
namespace settings {
namespace internal {

template <>
duration::Microseconds convert(const espurna::string& value) {
    return espurna::duration::unchecked_parse<duration::Microseconds>(value);
}

template <>
duration::Milliseconds convert(const espurna::string& value) {
    return espurna::duration::unchecked_parse<duration::Milliseconds>(value);
}

template <>
duration::Seconds convert(const espurna::string& value) {
    return espurna::duration::unchecked_parse<duration::Seconds>(value);
}

template <>
duration::Minutes convert(const espurna::string& value) {
    return espurna::duration::unchecked_parse<duration::Minutes>(value);
}

template <>
duration::Hours convert(const espurna::string& value) {
    return espurna::duration::unchecked_parse<duration::Hours>(value);
}

template <>
float convert(const espurna::string& value) {
    return espurna::parseDecimal(value.c_str());
}

template <>
double convert(const espurna::string& value) {
    return espurna::parseDecimal(value.c_str());
}

template <>
signed char convert(const espurna::string& value) {
    return value.toInt();
}

template <>
short convert(const espurna::string& value) {
    return value.toInt();
}

template <>
int convert(const espurna::string& value) {
    return value.toInt();
}

template <>
long convert(const espurna::string& value) {
    return value.toInt();
}

template <>
bool convert(const espurna::string& value) {
    if (value.length()) {
        if ((value == "0")
            || (value == "n")
            || (value == "no")
            || (value == "false")
            || (value == "off")) {
            return false;
        }

        return (value == "1")
            || (value == "y")
            || (value == "yes")
            || (value == "true")
            || (value == "on");
    }

    return false;
}

template <>
uint32_t convert(const espurna::string& value) {
    return parseUnsigned(value).value;
}

espurna::string serialize(uint32_t value, int base) {
    return formatUnsigned(value, base);
}

// the one of unsigned int / unsigned long that is not uint32_t (ref. OtherUnsigned)
template <>
OtherUnsigned convert(const espurna::string& value) {
    return convert<uint32_t>(value);
}

template <>
unsigned short convert(const espurna::string& value) {
    return convert<unsigned long>(value);
}

template <>
unsigned char convert(const espurna::string& value) {
    return convert<unsigned long>(value);
}

espurna::string serialize(duration::Seconds value) {
    return serialize(value.count());
}

espurna::string serialize(duration::Milliseconds value) {
    return serialize(value.count());
}

espurna::string serialize(duration::Minutes value) {
    return serialize(value.count()) + 'm';
}

espurna::string serialize(duration::Hours value) {
    return serialize(value.count()) + 'h';
}

} // namespace internal
} // namespace settings
} // namespace espurna

