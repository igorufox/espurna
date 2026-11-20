/*

SENSOR MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if SENSOR_SUPPORT

#include "sensor.h"

#include "api.h"
#include "domoticz.h"
#include "i2c.h"
#include "mqtt.h"
#include "ntp.h"
#include "relay.h"
#include "terminal.h"
#include "thingspeak.h"
#include "rtcmem.h"
#include "espurna_chip.h"
#include "ws.h"

#include <cfloat>
#include <cmath>
#include <cstring>

#include <limits>
#include <vector>

//--------------------------------------------------------------------------------

#include "sensors/BaseSensor.h"
#include "sensors/BaseEmonSensor.h"
#include "sensors/BaseAnalogEmonSensor.h"
#include "sensors/BaseAnalogSensor.h"

// the drivers are modules of their own (sensors/module_<name>.cpp, config/head.h): they register with
// sensorDriver() when they start and add their sensors with sensorAdd()

#include "filters/LastFilter.h"
#include "filters/MaxFilter.h"
#include "filters/MedianFilter.h"
#include "filters/MinFilter.h"
#include "filters/MovingAverageFilter.h"
#include "filters/SumFilter.h"


//--------------------------------------------------------------------------------

namespace espurna {
namespace sensor {

Value::operator bool() const {
    return !std::isinf(value) && !std::isnan(value);
}

namespace {

espurna::string error(unsigned char error) {
    const char* result { nullptr };

    switch (error) {
    case SENSOR_ERROR_OK:
        result = PSTR("OK");
        break;
    case SENSOR_ERROR_OUT_OF_RANGE:
        result = PSTR("Out of Range");
        break;
    case SENSOR_ERROR_WARM_UP:
        result = PSTR("Warming Up");
        break;
    case SENSOR_ERROR_TIMEOUT:
        result = PSTR("Timeout");
        break;
    case SENSOR_ERROR_UNKNOWN_ID:
        result = PSTR("Unknown ID");
        break;
    case SENSOR_ERROR_CRC:
        result = PSTR("CRC / Data Error");
        break;
    case SENSOR_ERROR_I2C:
        result = PSTR("I2C Error");
        break;
    case SENSOR_ERROR_GPIO_USED:
        result = PSTR("GPIO Already Used");
        break;
    case SENSOR_ERROR_CALIBRATION:
        result = PSTR("Calibration Error");
        break;
    case SENSOR_ERROR_OVERFLOW:
        result = PSTR("Value Overflow");
        break;
    case SENSOR_ERROR_NOT_READY:
        result = PSTR("Not Ready");
        break;
    case SENSOR_ERROR_CONFIG:
        result = PSTR("Invalid Configuration");
        break;
    case SENSOR_ERROR_SUPPORT:
        result = PSTR("Not Supported");
        break;
    case SENSOR_ERROR_NOT_FOUND:
        result = PSTR("Not found");
        break;
    case SENSOR_ERROR_VALUE:
        result = PSTR("Invalid Value");
        break;
    case SENSOR_ERROR_OTHER:
    default:
        result = PSTR("Other / Unknown Error");
        break;
    }

    return result;
}

template <typename T>
void forEachError(T&& callback) {
    for (unsigned char error = SENSOR_ERROR_OK; error < SENSOR_ERROR_MAX; ++error) {
        callback(error);
    }
}

// minimal subset of Value for internal use
struct ValuePair {
    double value;
    Unit units;
};

constexpr ValuePair make_value_pair(double value, Unit units) {
    return ValuePair{
        .value = value,
        .units = units,
    };
}

constexpr auto DefaultValuePair =
    make_value_pair(Value::Unknown, Unit::None);

} // namespace

namespace {

class BaseSensorPtr {
public:
    BaseSensorPtr() = delete;

    constexpr BaseSensorPtr(const BaseSensorPtr&) = default;
    constexpr BaseSensorPtr(BaseSensorPtr&&) noexcept = default;

#if __cplusplus > 201103L
    constexpr BaseSensorPtr& operator=(const BaseSensorPtr&) = default;
    constexpr BaseSensorPtr& operator=(BaseSensorPtr&&) noexcept = default;
#else
    BaseSensorPtr& operator=(const BaseSensorPtr&) = default;
    BaseSensorPtr& operator=(BaseSensorPtr&&) noexcept = default;
#endif

    constexpr BaseSensorPtr(std::nullptr_t) = delete;
    constexpr BaseSensorPtr& operator=(std::nullptr_t) = delete;

    constexpr BaseSensorPtr(BaseSensor* ptr) :
        _ptr(ptr)
    {}

    constexpr BaseSensor* get() const {
        return _ptr;
    }

    constexpr BaseSensor* operator->() const {
        return _ptr;
    }

private:
    BaseSensor* _ptr;
};

using BaseFilterPtr = std::unique_ptr<BaseFilter>;

// unique to the current runtime, for example .001.002.003.004
using Slot = std::array<char, 16>;

struct SlotValues {
    unsigned char id;
    unsigned char slot_id;
    unsigned char type;
    unsigned char slot;
};

constexpr char dec_digit2char(char c) {
    return ((c >= 0) && (c <= 9))
        ? ('0' + c)
        : '0';
}

#if __cplusplus >= 201603L
constexpr
#endif
std::array<char, 3> format_slot_number_impl(unsigned char number) {
    return std::array<char, 3>{
        number >= 100
            ? dec_digit2char(number / 100)
            : '0',
        number >= 10
            ? dec_digit2char((number / 10) % 10)
            : '0',
        number
            ? dec_digit2char(number % 10)
            : '0',
    };
}

// aka std::to_chars(it, it + 3, 10), prefixed with zeroes and a dot
// (unchecked for bounds, always assuming `format_slot` as caller)
template <typename T>
T append_slot_number_impl(T it, unsigned char value) {
    const auto raw = format_slot_number_impl(value);

    *(it++) = '.';
    *(it++) = raw[0];
    *(it++) = raw[1];
    *(it++) = raw[2];

    return it;
}

Slot make_slot(SlotValues values) {
    Slot out;

    auto it = out.begin();
    it = append_slot_number_impl(it, values.id);
    it = append_slot_number_impl(it, values.slot_id);
    it = append_slot_number_impl(it, values.type);
    append_slot_number_impl(it, values.slot);

    return out;
}

class Magnitude {
public:
    Magnitude() = delete;

    Magnitude(const Magnitude&) = delete;
    Magnitude& operator=(const Magnitude&) = delete;

    Magnitude(Magnitude&& other) noexcept = default;
    Magnitude& operator=(Magnitude&&) noexcept = default;

    Magnitude(BaseSensorPtr sensor) :
        sensor(std::move(sensor))
    {}

    BaseSensorPtr sensor; // Sensor object, *cannot be empty*

    unsigned char type; // Type of measurement, returned by the BaseSensor::type(slot)
    unsigned char slot; // Sensor slot # taken by the magnitude, used to access the measurement

    unsigned char index_global; // N'th magnitude of it's type, across all of the active sensors
    unsigned char slot_global; // Global slot aka index of the sensor, across all of the sensors with the same ID

    unsigned char slot_id; // Slot ID provided by the sensor. Expected to be varying between different instances of the sensor

    Unit units { Unit::None }; // Current units of measurement
    unsigned char decimals { 0u }; // Number of decimals in textual representation

    Filter filter_type; // Instead of using raw value, filter it through a filter object
    BaseFilterPtr filter; // *cannot be empty*, instance should be created based on the type above

    size_t read_count { 0 }; // Number of times 'last' was updated

    ValuePair last = DefaultValuePair; // Last 'read' value
    ValuePair reported = DefaultValuePair; // Last 'reported' value

    double min_delta { 0.0 }; // Minimum value change to report
    double max_delta { 0.0 }; // Maximum value change to report

    double min_threshold { Value::Unknown }; // Minimum value to report
    double max_threshold { Value::Unknown }; // Maximum value to report

    double zero_threshold { Value::Unknown }; // Reset value to zero when equal or below threshold (applied when reading)
    double correction { 0.0 }; // Value correction (applied when reading)
};

static_assert(
    std::is_nothrow_move_constructible<Magnitude>::value,
    "std::vector<Magnitude> should be able to work with resize()"
);

static_assert(
    !std::is_copy_constructible<Magnitude>::value,
    "std::vector<Magnitude> should only use move ctor"
);

bool isEmon(BaseSensorPtr sensor) {
    return (sensor->kind() == BaseEmonSensor::SensorKind())
        || (sensor->kind() == BaseAnalogEmonSensor::SensorKind());
}

bool isAnalogEmon(BaseSensorPtr sensor) {
    return sensor->kind() == BaseAnalogEmonSensor::SensorKind();
}

bool isAnalog(BaseSensorPtr sensor) {
    return sensor->kind() == BaseAnalogSensor::SensorKind();
}

namespace convert {
namespace temperature {

struct Base {
    constexpr Base() = default;
    constexpr explicit Base(double value) :
        _value(value)
    {}

    constexpr double value() const {
        return _value;
    }

    constexpr operator double() const {
        return _value;
    }

private:
    double _value { 0.0 };
};

struct Kelvin : public Base {
    using Base::Base;
};

struct Farenheit : public Base {
    using Base::Base;
};

struct Celcius : public Base {
    using Base::Base;
};

static constexpr Celcius AbsoluteZero { -273.15 };

namespace internal {

template <typename To, typename From, typename Same = void>
struct Converter {
};

template <typename To, typename From>
struct Converter<To, From, typename std::enable_if<std::is_same<To, From>::value>::type> {
    static constexpr To convert(To value) {
        return value;
    }
};

static constexpr double celcius_to_kelvin(double celcius) {
    return celcius - AbsoluteZero;
}

static constexpr double celcius_to_farenheit(double celcius) {
    return (celcius * (9.0 / 5.0)) + 32.0;
}

static constexpr double farenheit_to_celcius(double farenheit) {
    return (farenheit - 32.0) * (5.0 / 9.0);
}

static constexpr double farenheit_to_kelvin(double farenheit) {
    return celcius_to_kelvin(farenheit_to_celcius(farenheit));
}

static constexpr double kelvin_to_celcius(double kelvin) {
    return kelvin + AbsoluteZero;
}

static constexpr double kelvin_to_farenheit(double kelvin) {
    return celcius_to_farenheit(kelvin_to_celcius(kelvin));
}

static_assert(celcius_to_kelvin(kelvin_to_celcius(0.0)) == 0.0, "");
static_assert(celcius_to_farenheit(farenheit_to_celcius(0.0)) == 0.0, "");
static_assert(farenheit_to_kelvin(kelvin_to_farenheit(0.0)) == 0.0, "");
static_assert(farenheit_to_celcius(celcius_to_farenheit(0.0)) == 0.0, "");
static_assert(kelvin_to_celcius(celcius_to_kelvin(0.0)) == 0.0, "");

// ref. https://en.cppreference.com/w/cpp/types/numeric_limits/epsilon
static constexpr bool almost_equal(double lhs, double rhs, int ulp) {
    // the machine epsilon has to be scaled to the magnitude of the values used
    // and multiplied by the desired precision in ULPs (units in the last place)
    return __builtin_fabs(lhs - rhs) <= std::numeric_limits<double>::epsilon() * __builtin_fabs(lhs + rhs) * ulp
        // unless the result is subnormal
        || __builtin_fabs(lhs - rhs) < std::numeric_limits<double>::min();
}

static_assert(almost_equal(10.0, kelvin_to_farenheit(farenheit_to_kelvin(10.0)), 3), "");

template <>
struct Converter<Celcius, Kelvin> {
    static constexpr Celcius convert(Kelvin kelvin) {
        return Celcius{ kelvin_to_celcius(kelvin.value()) };
    }
};

template <>
struct Converter<Farenheit, Kelvin> {
    static constexpr Farenheit convert(Kelvin kelvin) {
        return Farenheit{ kelvin_to_farenheit(kelvin.value()) };
    }
};

template <>
struct Converter<Kelvin, Celcius> {
    static constexpr Kelvin convert(Celcius celcius) {
        return Kelvin{ celcius_to_kelvin(celcius.value()) };
    }
};

template <>
struct Converter<Farenheit, Celcius> {
    static constexpr Farenheit convert(Celcius celcius) {
        return Farenheit{ celcius_to_farenheit(celcius.value()) };
    }
};

template <>
struct Converter<Kelvin, Farenheit> {
    static constexpr Kelvin convert(Farenheit farenheit) {
        return Kelvin{ farenheit_to_kelvin(farenheit.value()) };
    }
};

template <>
struct Converter<Celcius, Farenheit> {
    static constexpr Celcius convert(Farenheit farenheit) {
        return Celcius{ farenheit_to_celcius(farenheit.value()) };
    }
};

// just some sanity checks. note that floating point will not always produce exact results
// (and it might not be a good idea to actually have anything compare with the Farenheit one)

static_assert(Converter<Kelvin, Kelvin>::convert(Kelvin{0.0}) == Kelvin{0.0}, "");
static_assert(Converter<Kelvin, Celcius>::convert(AbsoluteZero) == Kelvin{0.0}, "");
static_assert(Converter<Celcius, Celcius>::convert(AbsoluteZero) == AbsoluteZero, "");
static_assert(Converter<Celcius, Kelvin>::convert(Kelvin{0.0}) == AbsoluteZero, "");

} // namespace internal

template <typename To, typename From>
constexpr To unit_cast(From value) {
    return internal::Converter<To, From>::convert(value);
}

static_assert(unit_cast<Kelvin>(AbsoluteZero).value() == 0.0, "");
static_assert(unit_cast<Celcius>(AbsoluteZero).value() == AbsoluteZero.value(), "");

constexpr bool supported(Unit unit) {
    return (unit == Unit::Celcius)
        || (unit == Unit::Kelvin)
        || (unit == Unit::Farenheit);
}

// since the outside api only works with the enumeration, make sure to cast it to our types for conversion
// a table like this could've also worked
// > {Unit(from), Unit(to), Converter(double(*)(double))}
// but, it is ~0.6KiB vs. ~0.1KiB for this one. plus, some obstacles with c++11 implementation
// although, there may be a way to make this cheaper in both compile-time and runtime

// attempt to convert the input value from one unit to the other
// will return the input value when units match or there's no known conversion
constexpr double convert(double value, Unit from, Unit to) {
#define UNIT_CAST(LHS, RHS) \
    ((from == Unit::LHS) && (to == Unit::RHS)) \
        ? (unit_cast<RHS, LHS>(LHS{value})) : \
    ((from == Unit::RHS) && (to == Unit::LHS)) \
        ? (unit_cast<LHS, RHS>(RHS{value}))

     return UNIT_CAST(Kelvin, Celcius) :
        UNIT_CAST(Kelvin, Farenheit) :
        UNIT_CAST(Celcius, Farenheit) : value;

#undef UNIT_CAST
}

} // namespace temperature

// right now, limited to plain and kilo values
// (since we mostly care about a fairly small values)
// type conversion should only work for related types
namespace metric {

template <typename __Ratio>
struct Base {
    using Type = double;
    using Ratio = __Ratio;

    constexpr Base() = default;
    constexpr explicit Base(Type value) :
        _value(value)
    {}

    constexpr Type value() const {
        return _value;
    }

    constexpr operator Type() const {
        return _value;
    }

private:
    Type _value { 0.0 };
};

template <typename To, typename From>
struct convertible_base : std::false_type {
};

template <typename To, typename From>
constexpr bool is_convertible_base() {
    return std::is_same<To, From>::value
        || std::is_base_of<std::true_type, convertible_base<To, From>>::value
        || std::is_base_of<std::true_type, convertible_base<From, To>>::value;
}

template <typename To, typename From>
using is_convertible = std::enable_if<is_convertible_base<To, From>()>;

template <typename To, typename From,
          typename Divide = std::ratio_divide<typename From::Ratio, typename To::Ratio>,
          typename = typename is_convertible<To, From>::type>
constexpr To unit_cast(From value) {
    return To(value.value()
            * static_cast<typename To::Type>(Divide::num)
            / static_cast<typename To::Type>(Divide::den));
}

struct Watt : public Base<std::ratio<1, 1>> {
    using Base::Base;
};

struct Kilowatt : public Base<std::ratio<1000, 1>> {
    using Base::Base;
};

template <>
struct convertible_base<Watt, Kilowatt> : std::true_type {
};

struct Voltampere : public Base<std::ratio<1, 1>> {
    using Base::Base;
};

struct Kilovoltampere : public Base<std::ratio<1000, 1>> {
    using Base::Base;
};

template <>
struct convertible_base<Voltampere, Kilovoltampere> : std::true_type {
};

struct VoltampereReactive : public Base<std::ratio<1, 1>> {
    using Base::Base;
};

struct KilovoltampereReactive : public Base<std::ratio<1000, 1>> {
    using Base::Base;
};

template <>
struct convertible_base<VoltampereReactive, KilovoltampereReactive> : std::true_type {
};

struct WattSecond : public Base<std::ratio<1, 1>> {
    using Base::Base;
};

using Joule = WattSecond;

struct KilowattHour : public Base<std::ratio<3600000, 1>> {
    using Base::Base;
};

template <>
struct convertible_base<WattSecond, KilowattHour> : std::true_type {
};

static_assert(is_convertible_base<Voltampere, Kilovoltampere>(), "");
static_assert(is_convertible_base<Kilovoltampere, Voltampere>(), "");

static_assert(!is_convertible_base<KilovoltampereReactive, Voltampere>(), "");
static_assert(is_convertible_base<Joule, WattSecond>(), "");

static_assert(unit_cast<Joule>(KilowattHour{0.02}) == 72000.0, "");
static_assert(unit_cast<Joule>(KilowattHour{3.611111111111111e-05}) == 130.0, "");
static_assert(unit_cast<KilowattHour>(Joule{1080000.0}) == 0.3, "");
static_assert(unit_cast<KilowattHour>(Joule{12348000.0}) == 3.43, "");
static_assert(unit_cast<VoltampereReactive>(KilovoltampereReactive{1234.0}) == 1234000.0, "");

constexpr bool supported(Unit unit) {
    return (unit == Unit::Voltampere)
        || (unit == Unit::Kilovoltampere)
        || (unit == Unit::VoltampereReactive)
        || (unit == Unit::KilovoltampereReactive)
        || (unit == Unit::Watt)
        || (unit == Unit::Kilowatt)
        || (unit == Unit::Joule)
        || (unit == Unit::WattSecond)
        || (unit == Unit::KilowattHour);
}

// Here we only care about the direct counterparts
// Plus, we still don't enforce supported() at compile time,
// only safeguard is unit_cast<> failing for 'incompatible' base types

constexpr double convert(double value, Unit from, Unit to) {
#define UNIT_CAST(LHS, RHS) \
    ((from == Unit::LHS) && (to == Unit::RHS)) \
        ? (unit_cast<RHS, LHS>(LHS{value})) : \
    ((from == Unit::RHS) && (to == Unit::LHS)) \
        ? (unit_cast<LHS, RHS>(RHS{value}))

    return UNIT_CAST(Watt, Kilowatt) :
        UNIT_CAST(Voltampere, Kilovoltampere) :
        UNIT_CAST(VoltampereReactive, KilovoltampereReactive) :
        UNIT_CAST(Joule, KilowattHour) :
        UNIT_CAST(WattSecond, KilowattHour) : value;

#undef UNIT_CAST
}

} // namespace metric
} // namespace convert

namespace build {

constexpr double DefaultMinDelta { 0.0 };
constexpr double DefaultMaxDelta { 0.0 };

constexpr espurna::duration::Seconds initInterval() {
    return espurna::duration::Seconds(SENSOR_INIT_INTERVAL);
}

constexpr espurna::duration::Seconds ReadIntervalMin { SENSOR_READ_MIN_INTERVAL };
constexpr espurna::duration::Seconds ReadIntervalMax { SENSOR_READ_MAX_INTERVAL };

constexpr espurna::duration::Seconds readInterval() {
    return espurna::duration::Seconds(SENSOR_READ_INTERVAL);
}

constexpr size_t ReportEveryMin PROGMEM { SENSOR_REPORT_MIN_EVERY };
constexpr size_t ReportEveryMax PROGMEM { SENSOR_REPORT_MAX_EVERY };

constexpr size_t reportEvery() {
    return SENSOR_REPORT_EVERY;
}

constexpr size_t saveEvery() {
    return SENSOR_SAVE_EVERY;
}

constexpr bool realTimeValues() {
    return SENSOR_REAL_TIME_VALUES == 1;
}

constexpr bool useIndex() {
    return SENSOR_USE_INDEX == 1;
}

} // namespace build

namespace settings {
namespace filters {

PROGMEM_STRING(Last, "last");
PROGMEM_STRING(Max, "max");
PROGMEM_STRING(Median, "median");
PROGMEM_STRING(Min, "min");
PROGMEM_STRING(MovingAverage, "moving-average");
PROGMEM_STRING(Sum, "sum");

static constexpr espurna::settings::options::Enumeration<Filter> Options[] PROGMEM {
    {Filter::Last, Last},
    {Filter::Max, Max},
    {Filter::Median, Median},
    {Filter::Min, Min},
    {Filter::MovingAverage, MovingAverage},
    {Filter::Sum, Sum},
};

} // namespace filters

namespace units {

PROGMEM_STRING(Farenheit, "°F");
PROGMEM_STRING(Celcius, "°C");
PROGMEM_STRING(Kelvin, "K");
PROGMEM_STRING(Percentage, "%");
PROGMEM_STRING(Hectopascal, "hPa");
PROGMEM_STRING(Ampere, "A");
PROGMEM_STRING(Volt, "V");
PROGMEM_STRING(Watt, "W");
PROGMEM_STRING(Kilowatt, "kW");
PROGMEM_STRING(Voltampere, "VA");
PROGMEM_STRING(Kilovoltampere, "kVA");
PROGMEM_STRING(VoltampereReactive, "VAR");
PROGMEM_STRING(KilovoltampereReactive, "kVAR");
PROGMEM_STRING(Joule, "J");
PROGMEM_STRING(KilowattHour, "kWh");
PROGMEM_STRING(MicrogrammPerCubicMeter, "µg/m³");
PROGMEM_STRING(PartsPerMillion, "ppm");
PROGMEM_STRING(Lux, "lux");
PROGMEM_STRING(UltravioletIndex, "UVindex");
PROGMEM_STRING(Ohm, "ohm");
PROGMEM_STRING(MilligrammPerCubicMeter, "mg/m³");
PROGMEM_STRING(CountsPerMinute, "cpm");
PROGMEM_STRING(MicrosievertPerHour, "µSv/h");
PROGMEM_STRING(Meter, "m");
PROGMEM_STRING(Hertz, "Hz");
PROGMEM_STRING(Ph, "pH");
PROGMEM_STRING(None, "none");

static constexpr espurna::settings::options::Enumeration<Unit> Options[] PROGMEM {
    {Unit::Farenheit, Farenheit},
    {Unit::Celcius, Celcius},
    {Unit::Kelvin, Kelvin},
    {Unit::Percentage, Percentage},
    {Unit::Hectopascal, Hectopascal},
    {Unit::Ampere, Ampere},
    {Unit::Volt, Volt},
    {Unit::Watt, Watt},
    {Unit::Kilowatt, Kilowatt},
    {Unit::Voltampere, Voltampere},
    {Unit::Kilovoltampere, Kilovoltampere},
    {Unit::VoltampereReactive, VoltampereReactive},
    {Unit::KilovoltampereReactive, KilovoltampereReactive},
    {Unit::Joule, Joule},
    {Unit::WattSecond, Joule},
    {Unit::KilowattHour, KilowattHour},
    {Unit::MicrogrammPerCubicMeter, MicrogrammPerCubicMeter},
    {Unit::PartsPerMillion, PartsPerMillion},
    {Unit::Lux, Lux},
    {Unit::UltravioletIndex, UltravioletIndex},
    {Unit::Ohm, Ohm},
    {Unit::MilligrammPerCubicMeter, MilligrammPerCubicMeter},
    {Unit::CountsPerMinute, CountsPerMinute},
    {Unit::MicrosievertPerHour, MicrosievertPerHour},
    {Unit::Meter, Meter},
    {Unit::Hertz, Hertz},
    {Unit::Ph, Ph},
    {Unit::None, None},
};

} // namespace units

namespace prefix {

PROGMEM_STRING(Sensor, "sns");

PROGMEM_STRING(Temperature, "tmp");
PROGMEM_STRING(Humidity, "hum");
PROGMEM_STRING(Pressure, "press");
PROGMEM_STRING(Current, "curr");
PROGMEM_STRING(Voltage, "volt");
PROGMEM_STRING(PowerActive, "pwrP");
PROGMEM_STRING(PowerApparent, "pwrQ");
PROGMEM_STRING(PowerReactive, "pwrModS");
PROGMEM_STRING(PowerFactor, "pwrPF");
PROGMEM_STRING(Energy, "ene");
PROGMEM_STRING(EnergyDelta, "eneDelta");
PROGMEM_STRING(Analog, "analog");
PROGMEM_STRING(Digital, "digital");
PROGMEM_STRING(Event, "event");
PROGMEM_STRING(Pm1Dot0, "pm1dot0");
PROGMEM_STRING(Pm2Dot5, "pm2dot5");
PROGMEM_STRING(Pm10, "pm10");
PROGMEM_STRING(Co2, "co2");
PROGMEM_STRING(Voc, "voc");
PROGMEM_STRING(Iaq, "iaq");
PROGMEM_STRING(IaqAccuracy, "iaqAccuracy");
PROGMEM_STRING(IaqStatic, "iaqStatic");
PROGMEM_STRING(Lux, "lux");
PROGMEM_STRING(Uva, "uva");
PROGMEM_STRING(Uvb, "uvb");
PROGMEM_STRING(Uvi, "uvi");
PROGMEM_STRING(Distance, "distance");
PROGMEM_STRING(Hcho, "hcho");
PROGMEM_STRING(GeigerCpm, "gcpm");
PROGMEM_STRING(GeigerSievert, "gsiev");
PROGMEM_STRING(Count, "count");
PROGMEM_STRING(No2, "no2");
PROGMEM_STRING(Co, "co");
PROGMEM_STRING(Resistance, "res");
PROGMEM_STRING(Ph, "ph");
PROGMEM_STRING(Frequency, "freq");
PROGMEM_STRING(Tvoc, "tvoc");
PROGMEM_STRING(Ch2o, "ch2o");

PROGMEM_STRING(Unknown, "unknown");

constexpr StringView get(unsigned char type) {
    return (type == MAGNITUDE_TEMPERATURE) ? Temperature :
        (type == MAGNITUDE_HUMIDITY) ? Humidity :
        (type == MAGNITUDE_PRESSURE) ? Pressure :
        (type == MAGNITUDE_CURRENT) ? Current :
        (type == MAGNITUDE_VOLTAGE) ? Voltage :
        (type == MAGNITUDE_POWER_ACTIVE) ? PowerActive :
        (type == MAGNITUDE_POWER_APPARENT) ? PowerApparent :
        (type == MAGNITUDE_POWER_REACTIVE) ? PowerReactive :
        (type == MAGNITUDE_POWER_FACTOR) ? PowerFactor :
        (type == MAGNITUDE_ENERGY) ? Energy :
        (type == MAGNITUDE_ENERGY_DELTA) ? EnergyDelta :
        (type == MAGNITUDE_ANALOG) ? Analog :
        (type == MAGNITUDE_DIGITAL) ? Digital :
        (type == MAGNITUDE_EVENT) ? Event :
        (type == MAGNITUDE_PM1DOT0) ? Pm1Dot0 :
        (type == MAGNITUDE_PM2DOT5) ? Pm2Dot5 :
        (type == MAGNITUDE_PM10) ? Pm10 :
        (type == MAGNITUDE_CO2) ? Co2 :
        (type == MAGNITUDE_VOC) ? Voc :
        (type == MAGNITUDE_IAQ) ? Iaq :
        (type == MAGNITUDE_IAQ_ACCURACY) ? IaqAccuracy :
        (type == MAGNITUDE_IAQ_STATIC) ? IaqStatic :
        (type == MAGNITUDE_LUX) ? Lux :
        (type == MAGNITUDE_UVA) ? Uva :
        (type == MAGNITUDE_UVB) ? Uvb :
        (type == MAGNITUDE_UVI) ? Uvi :
        (type == MAGNITUDE_DISTANCE) ? Distance :
        (type == MAGNITUDE_HCHO) ? Hcho :
        (type == MAGNITUDE_GEIGER_CPM) ? GeigerCpm :
        (type == MAGNITUDE_GEIGER_SIEVERT) ? GeigerSievert :
        (type == MAGNITUDE_COUNT) ? Count :
        (type == MAGNITUDE_NO2) ? No2 :
        (type == MAGNITUDE_CO) ? Co :
        (type == MAGNITUDE_RESISTANCE) ? Resistance :
        (type == MAGNITUDE_PH) ? Ph :
        (type == MAGNITUDE_FREQUENCY) ? Frequency :
        (type == MAGNITUDE_TVOC) ? Tvoc :
        (type == MAGNITUDE_CH2O) ? Ch2o :
        Unknown;
}

} // namespace prefix

namespace suffix {

PROGMEM_STRING(Correction, "Correction");
PROGMEM_STRING(MaxDelta, "MaxDelta");
PROGMEM_STRING(MinDelta, "MinDelta");
PROGMEM_STRING(Precision, "Precision");
PROGMEM_STRING(Ratio, "Ratio");
PROGMEM_STRING(Units, "Units");

PROGMEM_STRING(MinThreshold, "MinThreshold");
PROGMEM_STRING(MaxThreshold, "MaxThreshold");

PROGMEM_STRING(ZeroThreshold, "ZeroThreshold");

PROGMEM_STRING(Mains, "Mains");
PROGMEM_STRING(Reference, "Reference");

PROGMEM_STRING(Total, "Total");

PROGMEM_STRING(Filter, "Filter");

} // namespace suffix

namespace keys {

PROGMEM_STRING(ReadInterval, "snsRead");
PROGMEM_STRING(InitInterval, "snsInit");
PROGMEM_STRING(ReportEvery, "snsReport");
PROGMEM_STRING(SaveEvery, "snsSave");
PROGMEM_STRING(RealTimeValues, "snsRealTime");

espurna::settings::Key get(espurna::StringView prefix, espurna::StringView suffix, size_t index) {
    espurna::string key;
    key.reserve(prefix.length() + suffix.length() + 4);
    key.concat(prefix.c_str(), prefix.length());
    key.concat(suffix.c_str(), suffix.length());

    return espurna::settings::Key(std::move(key), index);
}

espurna::settings::Key get(const Magnitude& magnitude, espurna::StringView suffix) {
    return get(prefix::get(magnitude.type), suffix, magnitude.index_global);
}

} // namespace keys

espurna::duration::Seconds readInterval() {
    return std::clamp(getSetting(keys::ReadInterval, build::readInterval()),
            build::ReadIntervalMin, build::ReadIntervalMax);
}

espurna::duration::Seconds initInterval() {
    return std::clamp(getSetting(keys::InitInterval, build::initInterval()),
            build::ReadIntervalMin, build::ReadIntervalMax);
}

size_t reportEvery() {
    return std::clamp(getSetting(keys::ReportEvery, build::reportEvery()),
            build::ReportEveryMin, build::ReportEveryMax);
}

int saveEvery() {
    return getSetting(keys::SaveEvery, build::saveEvery());
}

bool realTimeValues() {
    return getSetting(keys::RealTimeValues, build::realTimeValues());
}

} // namespace settings

} // namespace
} // namespace sensor

namespace settings {
namespace internal {

template <>
espurna::sensor::Unit convert(const espurna::string& value) {
    return convert(espurna::sensor::settings::units::Options, value,
            espurna::sensor::Unit::None);
}

espurna::string serialize(espurna::sensor::Unit unit) {
    return serialize(espurna::sensor::settings::units::Options, unit);
}

template <>
espurna::sensor::Filter convert(const espurna::string& value) {
    return convert(espurna::sensor::settings::filters::Options, value,
            espurna::sensor::Filter::Median);
}

espurna::string serialize(espurna::sensor::Filter filter) {
    return serialize(espurna::sensor::settings::filters::Options, filter);
}

} // namespace internal
} // namespace settings

namespace sensor {
namespace {

namespace magnitude {
namespace traits {

constexpr bool correction_supported(unsigned char type) {
  return (type == MAGNITUDE_TEMPERATURE)
      || (type == MAGNITUDE_HUMIDITY)
      || (type == MAGNITUDE_PRESSURE)
      || (type == MAGNITUDE_LUX);
}

static constexpr unsigned char ratio_types[] {
    MAGNITUDE_CURRENT,
    MAGNITUDE_VOLTAGE,
    MAGNITUDE_POWER_ACTIVE,
    MAGNITUDE_ENERGY,
};

constexpr bool ratio_supported(unsigned char type) {
    return (type == MAGNITUDE_CURRENT)
        || (type == MAGNITUDE_VOLTAGE)
        || (type == MAGNITUDE_POWER_ACTIVE)
        || (type == MAGNITUDE_ENERGY);
}

} // namespace traits

namespace internal {

unsigned char instance_count[SENSOR_ID_MAX]{};
unsigned char types_count[MAGNITUDE_MAX]{};

} // namespace internal

unsigned char instance_count_add(unsigned char id) {
    return internal::instance_count[id]++;
}

unsigned char types_count_add(unsigned char type) {
    return internal::types_count[type]++;
}

unsigned char types_count(unsigned char type) {
    return internal::types_count[type];
}

namespace build {

static constexpr double correction(unsigned char type) {
    return (
        (type == MAGNITUDE_TEMPERATURE) ? (SENSOR_TEMPERATURE_CORRECTION) :
        (type == MAGNITUDE_HUMIDITY) ? (SENSOR_HUMIDITY_CORRECTION) :
        (type == MAGNITUDE_LUX) ? (SENSOR_LUX_CORRECTION) :
        (type == MAGNITUDE_PRESSURE) ? (SENSOR_PRESSURE_CORRECTION) :
        0.0
    );
}

} // namespace build

espurna::string format(double value, unsigned char decimals) {
    return formatDecimalString(value, decimals);
}

espurna::string format(const Magnitude& magnitude, double value) {
    return format(value, magnitude.decimals);
}

espurna::string format(const Magnitude& magnitude, ValuePair value) {
    return format(magnitude, value.value);
}

espurna::string format_slot(const Magnitude& magnitude) {
    const auto slot = make_slot(
        SlotValues{
            .id = magnitude.sensor->id(),
            .slot_id = (magnitude.slot_id != 0)
                ? magnitude.slot_id
                : static_cast<unsigned char>(magnitude.slot_global + 1),
            .type = magnitude.type,
            .slot = static_cast<unsigned char>(magnitude.slot + 1),
        });

    const auto out = espurna::StringView(slot.data(), slot.size());

    return out.toString();
}

espurna::string name(unsigned char type) {
    const char* result = nullptr;

    switch (type) {
    case MAGNITUDE_TEMPERATURE:
        result = PSTR("Temperature");
        break;
    case MAGNITUDE_HUMIDITY:
        result = PSTR("Humidity");
        break;
    case MAGNITUDE_PRESSURE:
        result = PSTR("Pressure");
        break;
    case MAGNITUDE_CURRENT:
        result = PSTR("Current");
        break;
    case MAGNITUDE_VOLTAGE:
        result = PSTR("Voltage");
        break;
    case MAGNITUDE_POWER_ACTIVE:
        result = PSTR("Active Power");
        break;
    case MAGNITUDE_POWER_APPARENT:
        result = PSTR("Apparent Power");
        break;
    case MAGNITUDE_POWER_REACTIVE:
        result = PSTR("Reactive Power");
        break;
    case MAGNITUDE_POWER_FACTOR:
        result = PSTR("Power Factor");
        break;
    case MAGNITUDE_ENERGY:
        result = PSTR("Energy");
        break;
    case MAGNITUDE_ENERGY_DELTA:
        result = PSTR("Energy (delta)");
        break;
    case MAGNITUDE_ANALOG:
        result = PSTR("Analog");
        break;
    case MAGNITUDE_DIGITAL:
        result = PSTR("Digital");
        break;
    case MAGNITUDE_EVENT:
        result = PSTR("Event");
        break;
    case MAGNITUDE_PM1DOT0:
        result = PSTR("PM1.0");
        break;
    case MAGNITUDE_PM2DOT5:
        result = PSTR("PM2.5");
        break;
    case MAGNITUDE_PM10:
        result = PSTR("PM10");
        break;
    case MAGNITUDE_CO2:
        result = PSTR("CO2");
        break;
    case MAGNITUDE_VOC:
        result = PSTR("VOC");
        break;
    case MAGNITUDE_IAQ_STATIC:
        result = PSTR("IAQ (Static)");
        break;
    case MAGNITUDE_IAQ:
        result = PSTR("IAQ");
        break;
    case MAGNITUDE_IAQ_ACCURACY:
        result = PSTR("IAQ Accuracy");
        break;
    case MAGNITUDE_LUX:
        result = PSTR("Lux");
        break;
    case MAGNITUDE_UVA:
        result = PSTR("UVA");
        break;
    case MAGNITUDE_UVB:
        result = PSTR("UVB");
        break;
    case MAGNITUDE_UVI:
        result = PSTR("UVI");
        break;
    case MAGNITUDE_DISTANCE:
        result = PSTR("Distance");
        break;
    case MAGNITUDE_HCHO:
        result = PSTR("HCHO");
        break;
    case MAGNITUDE_GEIGER_CPM:
    case MAGNITUDE_GEIGER_SIEVERT:
        result = PSTR("Local Dose Rate");
        break;
    case MAGNITUDE_COUNT:
        result = PSTR("Count");
        break;
    case MAGNITUDE_NO2:
        result = PSTR("NO2");
        break;
    case MAGNITUDE_CO:
        result = PSTR("CO");
        break;
    case MAGNITUDE_RESISTANCE:
        result = PSTR("Resistance");
        break;
    case MAGNITUDE_PH:
        result = PSTR("pH");
        break;
    case MAGNITUDE_FREQUENCY:
        result = PSTR("Frequency");
        break;
    case MAGNITUDE_TVOC:
        result = PSTR("TVOC");
        break;
    case MAGNITUDE_CH2O:
        result = PSTR("CH2O");
        break;
    case MAGNITUDE_NONE:
    default:
        break;
    }

    return espurna::string(result);
}

espurna::string topic(unsigned char type) {
    const char* result = PSTR("unknown");

    switch (type) {
    case MAGNITUDE_TEMPERATURE:
        result = PSTR("temperature");
        break;
    case MAGNITUDE_HUMIDITY:
        result = PSTR("humidity");
        break;
    case MAGNITUDE_PRESSURE:
        result = PSTR("pressure");
        break;
    case MAGNITUDE_CURRENT:
        result = PSTR("current");
        break;
    case MAGNITUDE_VOLTAGE:
        result = PSTR("voltage");
        break;
    case MAGNITUDE_POWER_ACTIVE:
        result = PSTR("power");
        break;
    case MAGNITUDE_POWER_APPARENT:
        result = PSTR("apparent");
        break;
    case MAGNITUDE_POWER_REACTIVE:
        result = PSTR("reactive");
        break;
    case MAGNITUDE_POWER_FACTOR:
        result = PSTR("factor");
        break;
    case MAGNITUDE_ENERGY:
        result = PSTR("energy");
        break;
    case MAGNITUDE_ENERGY_DELTA:
        result = PSTR("energy_delta");
        break;
    case MAGNITUDE_ANALOG:
        result = PSTR("analog");
        break;
    case MAGNITUDE_DIGITAL:
        result = PSTR("digital");
        break;
    case MAGNITUDE_EVENT:
        result = PSTR("event");
        break;
    case MAGNITUDE_PM1DOT0:
        result = PSTR("pm1dot0");
        break;
    case MAGNITUDE_PM2DOT5:
        result = PSTR("pm2dot5");
        break;
    case MAGNITUDE_PM10:
        result = PSTR("pm10");
        break;
    case MAGNITUDE_CO2:
        result = PSTR("co2");
        break;
    case MAGNITUDE_VOC:
        result = PSTR("voc");
        break;
    case MAGNITUDE_IAQ:
        result = PSTR("iaq");
        break;
    case MAGNITUDE_IAQ_ACCURACY:
        result = PSTR("iaq_accuracy");
        break;
    case MAGNITUDE_IAQ_STATIC:
        result = PSTR("iaq_static");
        break;
    case MAGNITUDE_LUX:
        result = PSTR("lux");
        break;
    case MAGNITUDE_UVA:
        result = PSTR("uva");
        break;
    case MAGNITUDE_UVB:
        result = PSTR("uvb");
        break;
    case MAGNITUDE_UVI:
        result = PSTR("uvi");
        break;
    case MAGNITUDE_DISTANCE:
        result = PSTR("distance");
        break;
    case MAGNITUDE_HCHO:
        result = PSTR("hcho");
        break;
    case MAGNITUDE_GEIGER_CPM:
        result = PSTR("ldr_cpm"); // local dose rate [Counts per minute]
        break;
    case MAGNITUDE_GEIGER_SIEVERT:
        result = PSTR("ldr_uSvh"); // local dose rate [µSievert per hour]
        break;
    case MAGNITUDE_COUNT:
        result = PSTR("count");
        break;
    case MAGNITUDE_NO2:
        result = PSTR("no2");
        break;
    case MAGNITUDE_CO:
        result = PSTR("co");
        break;
    case MAGNITUDE_RESISTANCE:
        result = PSTR("resistance");
        break;
    case MAGNITUDE_PH:
        result = PSTR("ph");
        break;
    case MAGNITUDE_FREQUENCY:
        result = PSTR("frequency");
        break;
    case MAGNITUDE_TVOC:
        result = PSTR("tvoc");
        break;
    case MAGNITUDE_CH2O:
        result = PSTR("ch2o");
        break;
    case MAGNITUDE_NONE:
    default:
        break;
    }

    return espurna::string(result);
}

espurna::string topic(const Magnitude& magnitude) {
    return topic(magnitude.type);
}

espurna::string topicWithIndex(const Magnitude& magnitude) {
    auto out = topic(magnitude);
    if (sensor::build::useIndex() || (types_count(magnitude.type) > 1)) {
        out += '/' + espurna::string(magnitude.index_global, 10);
    }

    return out;
}

espurna::string description(const Magnitude& magnitude) {
    return magnitude.sensor->description(magnitude.slot);
}

sensor::Filter defaultFilter(unsigned char type) {
    switch (type) {
    case MAGNITUDE_IAQ:
    case MAGNITUDE_IAQ_STATIC:
    case MAGNITUDE_ENERGY:
        return Filter::Last;
    case MAGNITUDE_EVENT:
    case MAGNITUDE_DIGITAL:
        return Filter::Max;
    case MAGNITUDE_COUNT:
    case MAGNITUDE_ENERGY_DELTA:
        return Filter::Sum;
    case MAGNITUDE_GEIGER_CPM:
    case MAGNITUDE_GEIGER_SIEVERT:
        return Filter::MovingAverage;
    }

    return Filter::Median;
}

Filter defaultFilter(const Magnitude& magnitude) {
    return defaultFilter(magnitude.type);
}

BaseFilterPtr makeFilter(Filter filter) {
    BaseFilterPtr out;

    switch (filter) {
    case Filter::Last:
        out = std::make_unique<LastFilter>();
        break;
    case Filter::Max:
        out = std::make_unique<MaxFilter>();
        break;
    case Filter::Median:
        out = std::make_unique<MedianFilter>();
        break;
    case Filter::Min:
        out = std::make_unique<MinFilter>();
        break;
    case Filter::MovingAverage:
        out = std::make_unique<MovingAverageFilter>();
        break;
    case Filter::Sum:
        out = std::make_unique<SumFilter>();
        break;
    }

    return out;
}

// Hardcoded decimals for each magnitude
unsigned char decimals(Unit unit) {
    switch (unit) {
    case Unit::Celcius:
    case Unit::Farenheit:
        return 1;
    case Unit::Percentage:
        return 0;
    case Unit::Hectopascal:
        return 2;
    case Unit::Ampere:
        return 3;
    case Unit::Volt:
        return 0;
    case Unit::Watt:
    case Unit::Voltampere:
    case Unit::VoltampereReactive:
        return 0;
    case Unit::Kilowatt:
    case Unit::Kilovoltampere:
    case Unit::KilovoltampereReactive:
        return 3;
    case Unit::KilowattHour:
        return 3;
    case Unit::WattSecond:
        return 0;
    case Unit::CountsPerMinute:
    case Unit::MicrosievertPerHour:
        return 4;
    case Unit::Meter:
        return 3;
    case Unit::Hertz:
        return 1;
    case Unit::UltravioletIndex:
        return 3;
    case Unit::Ph:
        return 3;
    case Unit::None:
    default:
        break;
    }

    return 0;
}

// Process input (sensor) units and convert to the ones that magnitude specifies as output
ValuePair process(const Magnitude& magnitude, double value, Unit units) {
    auto out = make_value_pair(value, units);

    if (units != magnitude.units) {
        using namespace sensor::convert;
        if (temperature::supported(units) && temperature::supported(magnitude.units)) {
            out.value = temperature::convert(value, units, magnitude.units);
        } else if (metric::supported(units) && metric::supported(magnitude.units)) {
            out.value = metric::convert(value, units, magnitude.units);
        }
        out.units = magnitude.units;
    }

    // Input value might have more decimal points than necessary.
    out.value = roundTo(out.value, magnitude.decimals);

    return out;
}

ValuePair process(const Magnitude& magnitude, ValuePair value) {
    return process(magnitude, value.value,
        magnitude.sensor->units(magnitude.slot));
}

namespace internal {

std::vector<Magnitude> magnitudes;
bool real_time { sensor::build::realTimeValues() };

using ReadHandlers = std::forward_list<MagnitudeReadHandler>;
ReadHandlers read_handlers;
ReadHandlers report_handlers;

} // namespace internal

size_t count() {
    return internal::magnitudes.size();
}

Magnitude& add(BaseSensorPtr sensor, unsigned char type, unsigned char slot) {
    Magnitude out(sensor);

    out.type = type;
    out.slot = slot;
    out.index_global = types_count_add(type);
    out.slot_global = instance_count_add(sensor->id());
    out.slot_id = sensor->address_u8(slot);

    internal::magnitudes.emplace_back(std::move(out));
    return internal::magnitudes.back();
}

const Magnitude* find(unsigned char type, unsigned char index) {
    const Magnitude* out { nullptr };

    const auto result = std::find_if(
        std::cbegin(internal::magnitudes),
        std::cend(internal::magnitudes),
        [&](const Magnitude& magnitude) {
            return (magnitude.type == type) && (magnitude.index_global == index);
        });

    if (result != internal::magnitudes.end()) {
        out = std::addressof(*result);
    }

    return out;
}

unsigned char error(size_t index) {
    const auto& magnitude = internal::magnitudes[index];
    return magnitude.sensor->error();
}

Magnitude& get(size_t index) {
    return internal::magnitudes[index];
}

template <typename T>
void forEachInstance(T&& callback) {
    for (auto& magnitude : internal::magnitudes) {
        callback(magnitude);
    }
}

template <typename T>
void forEachCounted(T&& callback) {
    for (unsigned char type = MAGNITUDE_NONE + 1; type < MAGNITUDE_MAX; ++type) {
        if (types_count(type)) {
            callback(type);
        }
    }
}

// check if `callback(type)` returns `true` at least once
template <typename T>
bool forEachCountedCheck(T&& callback) {
    for (unsigned char type = MAGNITUDE_NONE + 1; type < MAGNITUDE_MAX; ++type) {
        if (types_count(type) && callback(type)) {
            return true;
        }
    }

    return false;
}

void onRead(MagnitudeReadHandler handler) {
    internal::read_handlers.push_front(handler);
}

void read(const Value& value) {
    for (auto& handler : internal::read_handlers) {
        handler(value);
    }
}

void onReport(MagnitudeReadHandler handler) {
    internal::report_handlers.push_front(handler);
}

void report(const Value& report) {
    for (auto& handler : internal::report_handlers) {
        handler(report);
    }
}

Info info(const Magnitude& magnitude) {
    return Info{
        .type = magnitude.type,
        .index = magnitude.index_global,
        .units = magnitude.units,
        .decimals = magnitude.decimals,
        .topic = topicWithIndex(magnitude),
        .description = espurna::string(),
    };
}

Value value(const Magnitude& magnitude, double value, Unit units) {
    return Value{
        .type = magnitude.type,
        .index = magnitude.index_global,
        .slot = format_slot(magnitude),
        .units = units,
        .decimals = magnitude.decimals,
        .topic = topicWithIndex(magnitude),
        .value = value,
        .repr = format(magnitude, value),
    };
}

Value value(const Magnitude& magnitude, ValuePair value) {
    return magnitude::value(magnitude, value.value, value.units);
}

template <typename T>
Value safe_value(size_t index, T&& retrieve) {
    Value out;
    out.value = Value::Unknown;

    if (index < count()) {
        const auto& magnitude = get(index);
        out = value(magnitude, retrieve(magnitude));
    }

    return out;
}

void prefer_real_time_values(bool real_time) {
    internal::real_time = real_time;
}

bool prefer_real_time_values() {
    return internal::real_time;
}

Value safe_value_last(size_t index) {
    return safe_value(
        index,
        [](const Magnitude& magnitude) {
            return magnitude.last;
        });
}

Value safe_value_reported(size_t index) {
    return safe_value(
        index,
        [](const Magnitude& magnitude) {
            return magnitude.reported;
        });
}

} // namespace magnitude

using TimeSource = espurna::time::CoreClock;

enum class State {
    None,
    Initial,
    Idle,
    Resume,
    Ready,
    Reading,
};

namespace notifications {
namespace internal {

std::forward_list<NotifyCallback> callbacks;
std::forward_list<timer::SystemTimer> timers;

} // namespace internal
} // namespace notifications
} // namespace

void notify_after_impl(duration::Milliseconds after, NotifyCallback callback) {
    using namespace notifications;

    internal::timers.push_front(timer::SystemTimer());

    auto& instance = internal::timers.front();
    instance.schedule_once(
        after,
        [callback]() {
            internal::callbacks.push_front(callback);
        });
}

void notify_now_impl(NotifyCallback callback) {
    notifications::internal::callbacks.push_front(callback);
}

namespace {
namespace internal {

std::vector<BaseSensorPtr> sensors;
std::vector<Driver> drivers;
size_t report_every { build::reportEvery() };

duration::Seconds read_interval { build::readInterval() };

std::forward_list<PreInitPtr> pre_init;
duration::Seconds init_interval { build::initInterval() };

} // namespace internal

size_t reportEvery() {
    return internal::report_every;
}

duration::Seconds readInterval() {
    return internal::read_interval;
}

duration::Seconds initInterval() {
    return internal::init_interval;
}

template <typename T>
void forEachInstance(T&& callback) {
    for (auto sensor : internal::sensors) {
        callback(sensor);
    }
}

void add(BaseSensor* sensor) {
    internal::sensors.push_back(sensor);
}

size_t count() {
    return internal::sensors.size();
}

namespace units {

using underlying_type = std::underlying_type<Unit>::type;

namespace internal {

constexpr auto CountMax = static_cast<underlying_type>(Unit::Max_) + 1;
unsigned char counts[CountMax];

} // namespace internal

struct Range {
    Range() = default;

    template <size_t Size>
    explicit Range(const Unit (&units)[Size]) :
        _begin(std::begin(units)),
        _end(std::end(units))
    {}

    template <size_t Size>
    Range& operator=(const Unit (&units)[Size]) {
        _begin = std::begin(units);
        _end = std::end(units);
        return *this;
    }

    const Unit* begin() const {
        return _begin;
    }

    const Unit* end() const {
        return _end;
    }

private:
    const Unit* _begin { nullptr };
    const Unit* _end { nullptr };
};

Range range(unsigned char type) {
#define MAGNITUDE_UNITS_RANGE(...)\
    static const Unit units[] PROGMEM {\
        __VA_ARGS__\
    };\
\
    out = units

    Range out;

    switch (type) {

    case MAGNITUDE_TEMPERATURE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Celcius,
            Unit::Farenheit,
            Unit::Kelvin
        );
        break;
    }

    case MAGNITUDE_HUMIDITY:
    case MAGNITUDE_POWER_FACTOR: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Percentage
        );
        break;
    }

    case MAGNITUDE_PRESSURE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Hectopascal
        );
        break;
    }

    case MAGNITUDE_CURRENT: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Ampere
        );
        break;
    }

    case MAGNITUDE_VOLTAGE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Volt
        );
        break;
    }

    case MAGNITUDE_POWER_ACTIVE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Watt,
            Unit::Kilowatt
        );
        break;
    }

    case MAGNITUDE_POWER_APPARENT: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Voltampere,
            Unit::Kilovoltampere
        );
        break;
    }

    case MAGNITUDE_POWER_REACTIVE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::VoltampereReactive,
            Unit::KilovoltampereReactive
        );
        break;
    }

    case MAGNITUDE_ENERGY_DELTA: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Joule
        );
        break;
    }

    case MAGNITUDE_ENERGY: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Joule,
            Unit::KilowattHour
        );
        break;
    }

    case MAGNITUDE_PM1DOT0:
    case MAGNITUDE_PM2DOT5:
    case MAGNITUDE_PM10:
    case MAGNITUDE_TVOC:
    case MAGNITUDE_CH2O: {
        MAGNITUDE_UNITS_RANGE(
            Unit::MicrogrammPerCubicMeter,
            Unit::MilligrammPerCubicMeter
        );
        break;
    }

    case MAGNITUDE_CO:
    case MAGNITUDE_CO2:
    case MAGNITUDE_NO2:
    case MAGNITUDE_VOC: {
        MAGNITUDE_UNITS_RANGE(
            Unit::PartsPerMillion
        );
        break;
    }

    case MAGNITUDE_LUX: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Lux
        );
        break;
    }

    case MAGNITUDE_RESISTANCE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Ohm
        );
        break;
    }

    case MAGNITUDE_HCHO: {
        MAGNITUDE_UNITS_RANGE(
            Unit::MilligrammPerCubicMeter
        );
        break;
    }

    case MAGNITUDE_GEIGER_CPM: {
        MAGNITUDE_UNITS_RANGE(
            Unit::CountsPerMinute
        );
        break;
    }

    case MAGNITUDE_GEIGER_SIEVERT: {
        MAGNITUDE_UNITS_RANGE(
            Unit::MicrosievertPerHour
        );
        break;
    }

    case MAGNITUDE_DISTANCE: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Meter
        );
        break;
    }

    case MAGNITUDE_FREQUENCY: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Hertz
        );
        break;
    }

    case MAGNITUDE_PH: {
        MAGNITUDE_UNITS_RANGE(
            Unit::Ph
        );
        break;
    }

    }

#undef MAGNITUDE_UNITS_RANGE
    return out;
}

bool supported(const Magnitude& magnitude, Unit unit) {
    const auto range = units::range(magnitude.type);
    return std::any_of(range.begin(), range.end(), [&](sensor::Unit supported) {
        return (unit == supported);
    });
}

sensor::Unit filter(const Magnitude& magnitude, Unit unit) {
    return supported(magnitude, unit) ? unit : magnitude.units;
}

size_t count(Unit unit) {
    return internal::counts[static_cast<underlying_type>(unit)];
}

template <typename T>
void forEachCounted(T&& callback) {
    for (unsigned char index = 0; index < internal::CountMax; ++index) {
        if (internal::counts[index] > 0) {
            callback(index, static_cast<Unit>(index));
        }
    }
}

espurna::string name(Unit unit) {
    return espurna::settings::internal::serialize(unit);
}

espurna::string name(const Magnitude& magnitude) {
    return name(magnitude.units);
}

void setup(Unit unit) {
    ++internal::counts[static_cast<underlying_type>(unit)];
}

void setup(unsigned char type) {
    const auto range = units::range(type);
    for (auto entry : range) {
        setup(entry);
    }
}

} // namespace units

namespace magnitude {

espurna::string format_with_units(const Magnitude& magnitude, const ValuePair& pair) {
    espurna::string out;

    if (std::isnan(pair.value)) {
        out += STRING_VIEW("nan").toString();
        return out;
    }

    out += magnitude::format(magnitude, pair.value);
    if (pair.units != Unit::None) {
        out += units::name(pair.units);
    }

    return out;
}

} // namespace magnitude

// -----------------------------------------------------------------------------
// Energy persistence
// -----------------------------------------------------------------------------

namespace energy {

struct Persist {
    Persist(size_t index, Energy energy) :
        _index(index),
        _energy(energy)
    {}

    void operator()() const {
        setSetting({"eneTotal", _index}, _energy.asString());
#if NTP_SUPPORT
        if (ntpSynced()) {
            setSetting({"eneTime", _index}, ntpDateTime());
        }
#endif
    }

private:
    size_t _index;
    Energy _energy;
};

struct Tracker {
    using Reference = std::reference_wrapper<const Magnitude>;

    struct Counter {
        Reference magnitude;
        int value;
    };

    using Counters = std::vector<Counter>;

    explicit operator bool() const {
        return _every > 0;
    }

    int every() const {
        return _every;
    }

    void add(Reference magnitude) {
        _count.push_back(Counter{
            .magnitude = magnitude,
            .value = 0
        });
    }

    size_t size() const {
        return _count.size();
    }

    int count(size_t index) const {
        return _count[index].value;
    }

    template <typename Callback>
    void tick(unsigned char index, Callback&& callback) {
        _count[index].value = (_count[index].value + 1) % _every;
        if (_count[index].value == 0) {
            callback();
        }
    }

    void every(int every) {
        _every = every;
        for (auto& count : _count) {
            count.value = 0;
        }
    }

private:
    Counters _count;
    int _every;
};

struct ParseResult {
    ParseResult() = default;
    ParseResult& operator=(sensor::Energy value) {
        _value = value;
        _result = true;
        return *this;
    }

    explicit operator bool() const {
        return _result;
    }

    Energy value() const {
        return _value;
    }

private:
    bool _result { false };
    Energy _value;
};

namespace internal {

Tracker tracker;

} // namespace internal

Energy get_rtcmem(unsigned char index) {
    return Energy {
        Energy::Pair {
            .kwh = KilowattHours(Rtcmem->energy[index].kwh),
            .ws = WattSeconds(Rtcmem->energy[index].ws),
        }};
}

void set_rtcmem(unsigned char index, const Energy& source) {
    const auto pair = source.pair();
    Rtcmem->energy[index].kwh = pair.kwh.value;
    Rtcmem->energy[index].ws = pair.ws.value;
}


ParseResult convert(StringView value) {
    ParseResult out;
    if (!value.length()) {
        return out;
    }

    const auto begin = value.begin();
    const auto end = value.end();

    espurna::string kwh_number;

    auto it = begin;
    while (it != end) {
        if (*it == '+') {
            break;
        }

        kwh_number += *it;
        ++it;
    }

    KilowattHours::Type kwh { 0 };
    WattSeconds::Type ws { 0 };

    const auto result = parseUnsigned(kwh_number, 10);
    if (!result.ok) {
        return out;
    }

    kwh = result.value;

    if ((it != end) && (*it == '+')) {
        ++it;
        if (it == end) {
            return out;
        }

        const auto result = parseUnsigned(
                espurna::StringView(it, end), 10);
        if (!result.ok) {
            return out;
        }

        ws = result.value;
    }

    out = Energy {
        Energy::Pair {
            .kwh = KilowattHours(kwh),
            .ws = WattSeconds(ws),
        }};

    return out;
}

Energy get_settings(unsigned char index) {
    using namespace settings;
    const auto current = getSetting(
        keys::get(prefix::get(MAGNITUDE_ENERGY), suffix::Total, index));
    return convert(current).value();
}

void set(const Magnitude& magnitude, const Energy& energy) {
    if (isEmon(magnitude.sensor)) {
        auto* sensor = static_cast<BaseEmonSensor*>(magnitude.sensor.get());
        sensor->resetEnergy(magnitude.slot, energy);
    }
}

void set(const Magnitude& magnitude, StringView payload) {
    if (!payload.length()) {
        return;
    }

    auto energy = convert(payload);
    if (!energy) {
        return;
    }

    set(magnitude, energy.value());
}

Energy get(unsigned char index) {
    Energy result;

    if (rtcmemStatus() && (index < (sizeof(Rtcmem->energy) / sizeof(*Rtcmem->energy)))) {
        result = get_rtcmem(index);
    } else {
        result = get_settings(index);
    }

    return result;
}

void reset(unsigned char index) {
    delSetting({"eneTotal", index});
    delSetting({"eneTime", index});
    if (index < (sizeof(Rtcmem->energy) / sizeof(*Rtcmem->energy))) {
        Rtcmem->energy[index].kwh = 0;
        Rtcmem->energy[index].ws = 0;
    }
}

int every() {
    return internal::tracker.every();
}

void every(int value) {
    internal::tracker.every(value);
}

void update(const Magnitude& magnitude, bool persistent) {
    if (!isEmon(magnitude.sensor)) {
        return;
    }

    auto* sensor = static_cast<BaseEmonSensor*>(magnitude.sensor.get());
    const auto energy = sensor->totalEnergy(magnitude.slot);

    // Always save to RTCMEM
    if (magnitude.index_global < (sizeof(Rtcmem->energy) / sizeof(*Rtcmem->energy))) {
        set_rtcmem(magnitude.index_global, energy);
    }

    // Save to EEPROM every '_sensor_save_every' readings
    if (persistent && internal::tracker) {
        internal::tracker.tick(magnitude.index_global,
            Persist{magnitude.index_global, energy});
    }
}

void reset() {
    for (auto type : magnitude::traits::ratio_types) {
        for (size_t index = 0; index < magnitude::types_count(type); ++index) {
            delSetting(settings::keys::get(settings::prefix::get(type), settings::suffix::Ratio, index));
        }
    }

    for (auto ptr : sensor::internal::sensors) {
        if (isEmon(ptr)) {
            DEBUG_MSG_P(PSTR("[EMON] Resetting %s\n"), ptr->description().c_str());
            static_cast<BaseEmonSensor*>(ptr.get())->resetRatios();
        }
    }
}

double ratioFromValue(const Magnitude& magnitude, double expected) {
    if (!isEmon(magnitude.sensor)) {
        return BaseEmonSensor::DefaultRatio;
    }

    auto* sensor = static_cast<BaseEmonSensor*>(magnitude.sensor.get());
    return sensor->ratioFromValue(magnitude.slot, sensor->value(magnitude.slot), expected);
}

void setup(const Magnitude& magnitude) {
    if (!isEmon(magnitude.sensor)) {
        return;
    }

    auto* sensor = static_cast<BaseEmonSensor*>(magnitude.sensor.get());
    sensor->initialEnergy(magnitude.slot, get(magnitude.index_global));
    internal::tracker.add(magnitude);

    DEBUG_MSG_P(PSTR("[ENERGY] Tracking %s/%u for %s\n"),
            magnitude::topic(magnitude).c_str(),
            magnitude.index_global,
            magnitude.sensor->description().c_str());
}

} // namespace energy

namespace settings {
namespace query {

#define EXACT_VALUE(NAME, FUNC)\
espurna::string NAME () {\
    return espurna::settings::internal::serialize(FUNC());\
}

EXACT_VALUE(readInterval, settings::readInterval);
EXACT_VALUE(initInterval, settings::initInterval);
EXACT_VALUE(reportEvery, settings::reportEvery);
EXACT_VALUE(saveEvery, settings::saveEvery);
EXACT_VALUE(realTimeValues, settings::realTimeValues);

static constexpr espurna::settings::query::Setting Settings[] {
    {keys::ReadInterval, readInterval},
    {keys::InitInterval, initInterval},
    {keys::ReportEvery, reportEvery},
    {keys::SaveEvery, saveEvery},
    {keys::RealTimeValues, realTimeValues},
};

#undef EXACT_VALUE

namespace getter {

#define EXACT_VALUE(NAME)\
espurna::string NAME (const Magnitude& magnitude) {\
    return espurna::settings::internal::serialize(magnitude.NAME);\
}

struct Type {
    using Check = bool(*)(unsigned char);
    using Get = espurna::string(*)(const Magnitude&);

    StringView suffix;
    Check check;
    Get get;
};

EXACT_VALUE(correction)
EXACT_VALUE(decimals)
EXACT_VALUE(filter_type)

espurna::string ratio(const Magnitude& magnitude) {
    const auto ptr = reinterpret_cast<BaseEmonSensor*>(magnitude.sensor.get());
    return espurna::string(ptr->defaultRatio(magnitude.slot));
}

EXACT_VALUE(units)

#undef EXACT_VALUE

static constexpr std::array<Type, 5> List PROGMEM {{
    {suffix::Correction, magnitude::traits::correction_supported, correction},
    {suffix::Filter, nullptr, filter_type},
    {suffix::Precision, nullptr, decimals},
    {suffix::Ratio, magnitude::traits::ratio_supported, ratio},
    {suffix::Units, nullptr, units},
}};

} // namespace getter

using espurna::settings::query::Result;

bool checkSensor(StringView key) {
    return key.startsWith(settings::prefix::Sensor);
}

Result findFrom(StringView key) {
    return espurna::settings::query::findFrom(Settings, key);
}

bool checkMagnitude(StringView key) {
    if (key.length() < 3) {
        return false;
    }

    return magnitude::forEachCountedCheck(
        [&](unsigned char type) {
            return key.startsWith(prefix::get(type));
        });
}

Result findMagnitudeFrom(StringView key) {
    for (auto& magnitude : magnitude::internal::magnitudes) {
        for (const auto& type : getter::List) {
            if (type.check && !type.check(magnitude.type)) {
                continue;
            }

            const auto expected = keys::get(magnitude, type.suffix);
            if (key == expected.value()) {
                return Result(type.get(magnitude));
            }
        }
    }

    return Result(nullptr);
}

bool check(StringView key) {
    return checkSensor(key) || checkMagnitude(key);
}

void setup() {
    settingsRegisterQueryHandler({
        .check = checkMagnitude,
        .get = findMagnitudeFrom,
    });

    settingsRegisterQueryHandler({
        .check = checkSensor,
        .get = findFrom,
    });
}

} // namespace query

void migrate(int version) {
    auto firstKey = [](unsigned char type, StringView suffix)  {
        return keys::get(prefix::get(type), suffix, 0).value();
    };

    // Some keys from older versions were longer
    if (version < 3) {
        moveSetting("powerUnits", "pwrUnits");
        moveSetting("energyUnits", "eneUnits");
    }

    // Energy is now indexed (based on magnitude.index_global)
	// Also update PZEM004T energy total across multiple devices
    if (version < 5) {
        moveSetting("eneTotal", firstKey(MAGNITUDE_ENERGY, suffix::Total));
        moveSettings("pzEneTotal", prefix::get(MAGNITUDE_ENERGY).toString() + espurna::StringView(suffix::Total).toString());
    }

    // Unit ID is no longer shared, drop when equal to Min_ or None
    if (version < 5) {
        delSetting("pwrUnits");
        delSetting("eneUnits");
        delSetting("tmpUnits");
    }

    // Generic pwr settings now have type-specific prefixes
    // (index 0, assuming there's only one emon sensor)
    if (version < 7) {
        moveSetting("pwrVoltage", firstKey(MAGNITUDE_VOLTAGE, suffix::Mains));
        moveSetting("pwrRatioC", firstKey(MAGNITUDE_CURRENT, suffix::Ratio));
        moveSetting("pwrRatioV", firstKey(MAGNITUDE_VOLTAGE, suffix::Ratio));
        moveSetting("pwrRatioP", firstKey(MAGNITUDE_POWER_ACTIVE, suffix::Ratio));
        moveSetting("pwrRatioE", firstKey(MAGNITUDE_ENERGY, suffix::Ratio));
    }

#if HLW8012_SUPPORT
    if (version < 9) {
        moveSetting("snsHlw8012SelGPIO", "hlw8012SEL");
        moveSetting("snsHlw8012CfGPIO", "hlw8012CF");
        moveSetting("snsHlw8012Cf1GPIO", "hlw8012CF1");
    }
#endif

    if (version < 11) {
        moveSetting("apiRealTime", keys::RealTimeValues);
        moveSetting("tmpMinDelta", firstKey(MAGNITUDE_TEMPERATURE, suffix::MinDelta));
        moveSetting("humMinDelta", firstKey(MAGNITUDE_HUMIDITY, suffix::MinDelta));
        moveSetting("eneMaxDelta", firstKey(MAGNITUDE_ENERGY, suffix::MaxDelta));
    }
}

} // namespace settings

// -----------------------------------------------------------------------------
// WebUI value display and actions
// -----------------------------------------------------------------------------

#if WEB_SUPPORT
namespace {

// every settings key of a driver: the first part of each field of its spec ("key|type|label|default|options|repeat"),
// a '#' in a key is the index 0 .. repeat - 1
template <typename T>
void for_each_driver_key(const Driver& driver, T&& callback) {
    const espurna::string spec(driver.spec);

    int start = 0;
    while (start < static_cast<int>(spec.length())) {
        int end = spec.indexOf(';', start);
        if (end < 0) {
            end = spec.length();
        }

        const espurna::string field = spec.substring(start, end);
        start = end + 1;

        espurna::string parts[6];
        int from = 0;
        for (auto& part : parts) {
            const int to = field.indexOf('|', from);
            part = field.substring(from, (to < 0) ? field.length() : to);
            if (to < 0) {
                break;
            }
            from = to + 1;
        }

        const auto& key = parts[0];
        if (!key.length()) {
            continue;
        }

        const int hash = key.indexOf('#');
        if (hash < 0) {
            callback(key);
            continue;
        }

        const long repeat = parts[5].toInt();
        for (long index = 0; index < repeat; ++index) {
            espurna::string out = key.substring(0, hash);
            out += espurna::string(index);
            out += key.substring(hash + 1);
            callback(out);
        }
    }
}

[[gnu::unused]]
bool driver_key(StringView key) {
    for (const auto& driver : internal::drivers) {
        bool found { false };
        for_each_driver_key(driver, [&](const espurna::string& name) {
            found = found || (key == espurna::StringView(name));
        });
        if (found) {
            return true;
        }
    }

    return false;
}

} // namespace

namespace web {

bool onKeyCheck(StringView key, espurna::json::Value) {
    return settings::query::check(key) || driver_key(key);
}

// the drivers and their settings, for the SENSORS panel: the spec as the driver wrote it (the WebUI parses it,
// ref. sensors/driver.h) and the stored values of its keys (empty - not set, the WebUI shows the default)
void drivers(espurna::json::Writer& root) {
    root.array(STRING_VIEW("snsDrivers"));
    for (const auto& driver : internal::drivers) {
        root.beginObject();
        root.field(STRING_VIEW("name"), driver.name);
        root.field(STRING_VIEW("spec"), driver.spec);

        root.object(STRING_VIEW("values"));
        for_each_driver_key(driver, [&](const espurna::string& key) {
            root.field(espurna::StringView(key), getSetting(key));
        });
        root.end();
        root.end();
    }
    root.end();
}

// Entries related to things reported by the module.
// - types of magnitudes that are available and the string values associated with them
// - error types and stringified versions of them
// - units are the value types of the magnitude
// TODO: magnitude types have some common keys and some specific ones, only implemented for the type
// e.g. voltMains is specific to the MAGNITUDE_VOLTAGE but *only* in analog mode, or eneRatio specific to MAGNITUDE_ENERGY
// but, notice that the sensor will probably be used to 'get' certain properties, to generate certain keys list

void types(espurna::json::Writer& root) {
    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("types")};
    payload(STRING_VIEW("values"), {MAGNITUDE_NONE + 1, MAGNITUDE_MAX},
        [](size_t type) {
            return magnitude::types_count(type) > 0;
        },
        {{STRING_VIEW("type"), [](espurna::json::Writer& out, size_t index) {
            out.value(index);
        }},
        {STRING_VIEW("prefix"), [](espurna::json::Writer& out, size_t index) {
            out.value(settings::prefix::get(index));
        }},
        {STRING_VIEW("name"), [](espurna::json::Writer& out, size_t index) {
            out.value(sensor::magnitude::name(index));
        }},
        {STRING_VIEW("units"), [](espurna::json::Writer& out, size_t index) {
            out.beginArray();
            const auto range = units::range(index);
            for (auto entry : range) {
                out.value(static_cast<units::underlying_type>(entry));
            }
            out.end();
        }},
    });
}

void errors(espurna::json::Writer& root) {
    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("errors")};
    payload(STRING_VIEW("values"), SENSOR_ERROR_MAX,
        {{STRING_VIEW("type"), [](espurna::json::Writer& out, size_t index) {
            out.value(index);
        }},
        {STRING_VIEW("name"), [](espurna::json::Writer& out, size_t index) {
            out.value(error(index));
        }}
    });
}

void units(espurna::json::Writer& root) {
    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("units")};
    payload(STRING_VIEW("values"),
        {
            static_cast<size_t>(Unit::Min_),
            static_cast<size_t>(Unit::Max_) + 1,
        },
        [](size_t type) {
            return units::count(static_cast<Unit>(type)) > 0;
        },
        {{STRING_VIEW("type"), [](espurna::json::Writer& out, size_t index) {
            out.value(index);
        }},
        {STRING_VIEW("name"), [](espurna::json::Writer& out, size_t index) {
            out.value(sensor::units::name(static_cast<Unit>(index)));
        }}
    });
}

void initial(espurna::json::Writer& root) {
    if (!sensor::ready()) {
        root.field(STRING_VIEW("magnitudes-pending"), 1);
        return;
    }

    root.object(STRING_VIEW("magnitudes-init"));
    types(root);
    errors(root);
    units(root);
    root.end();
}

void list(espurna::json::Writer& root) {
    if (!sensor::ready()) {
        return;
    }

    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("magnitudes-list")};
    payload(STRING_VIEW("values"), magnitude::count(),
        {{STRING_VIEW("type"), [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::get(index).type);
        }},
        {STRING_VIEW("index_global"), [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::get(index).index_global);
        }},
        {STRING_VIEW("description"), [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::description(magnitude::get(index)));
        }},
        {STRING_VIEW("units"), [](espurna::json::Writer& out, size_t index) {
            out.value(static_cast<int>(magnitude::get(index).units));
        }}
    });
}

void threshold_or_nan(espurna::json::Writer& out, const double& threshold) {
    if (!std::isnan(threshold)) {
        out.value(threshold);
    } else {
        out.value("NaN");
    }
}

void settings(espurna::json::Writer& root) {
    if (!sensor::ready()) {
        return;
    }

    // XXX: inject 'null' in the output. need this for optional fields, since the current
    // version of serializer only does this for char ptr and even makes NaN serialized as
    // NaN, instead of more commonly used null (but, expect this to be fixed after switching to v6+)
    constexpr char* const NullSymbol { nullptr };

    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("magnitudes-settings")};
    payload(STRING_VIEW("values"), magnitude::count(),
        {{settings::suffix::Correction, [](espurna::json::Writer& out, size_t index) {
            const auto& magnitude = magnitude::get(index);
            if (magnitude::traits::correction_supported(magnitude.type)) {
                out.value(magnitude.correction);
            } else {
                out.value(NullSymbol);
            }
        }},
        {settings::suffix::Ratio, [](espurna::json::Writer& out, size_t index) {
            const auto& magnitude = magnitude::get(index);
            if (isEmon(magnitude.sensor) && magnitude::traits::ratio_supported(magnitude.type)) {
                out.value(static_cast<BaseEmonSensor*>(magnitude.sensor.get())->getRatio(magnitude.slot));
            } else {
                out.value(NullSymbol);
            }
        }},
        {settings::suffix::ZeroThreshold, [](espurna::json::Writer& out, size_t index) {
            const auto threshold = magnitude::get(index).zero_threshold;
            threshold_or_nan(out, threshold);
        }},
        {settings::suffix::MinThreshold, [](espurna::json::Writer& out, size_t index) {
            const auto threshold = magnitude::get(index).min_threshold;
            threshold_or_nan(out, threshold);
        }},
        {settings::suffix::MaxThreshold, [](espurna::json::Writer& out, size_t index) {
            const auto threshold = magnitude::get(index).max_threshold;
            threshold_or_nan(out, threshold);
        }},
        {settings::suffix::MinDelta, [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::get(index).min_delta);
        }},
        {settings::suffix::MaxDelta, [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::get(index).max_delta);
        }}
    });

    root.field(settings::keys::RealTimeValues, magnitude::prefer_real_time_values());
    root.field(settings::keys::ReadInterval, readInterval().count());
    root.field(settings::keys::InitInterval, initInterval().count());
    root.field(settings::keys::ReportEvery, reportEvery());
    root.field(settings::keys::SaveEvery, energy::internal::tracker.every());
}

void energy(espurna::json::Writer& root) {
#if NTP_SUPPORT
    if (!energy::internal::tracker || !energy::internal::tracker.size()) {
        return;
    }

    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("energy")};
    payload(STRING_VIEW("values"), espurna::settings::Iota(magnitude::count()),
        [](size_t index) {
            return magnitude::get(index).type == MAGNITUDE_ENERGY;
        },
        {{STRING_VIEW("id"), [](espurna::json::Writer& out, size_t index) {
            out.value(index);
        }},
        {STRING_VIEW("saved"), [](espurna::json::Writer& out, size_t index) {
            if (energy::internal::tracker) {
                out.value(getSetting({"eneTime", magnitude::get(index).index_global}, "(unknown)"));
            } else {
                out.value("");
            }
        }}
    });
#endif
}

void magnitudes(espurna::json::Writer& root) {
    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("magnitudes")};
    payload(STRING_VIEW("values"), magnitude::count(), {
        {STRING_VIEW("value"), [](espurna::json::Writer& out, size_t index) {
            const auto& magnitude = magnitude::get(index);
            out.value(magnitude::format(magnitude, magnitude.last));
        }},
        {STRING_VIEW("units"), [](espurna::json::Writer& out, size_t index) {
            const auto& magnitude = magnitude::get(index);
            out.value(static_cast<int>(magnitude.last.units));
        }},
        {STRING_VIEW("error"), [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::error(index));
        }},
    });
}

void onData(espurna::json::Writer& root) {
    if (magnitude::count()) {
        magnitudes(root);
        energy(root);
    }
}

void onAction(uint32_t client_id, const char* action, espurna::json::Value data) {
    if (STRING_VIEW("emon-expected") == action) {
        auto id = static_cast<size_t>(data["id"].asULong());
        if (id < magnitude::count()) {
            auto expected = data["expected"].asDouble();
            wsPost(client_id, [id, expected](espurna::json::Writer& root) {
                const auto& magnitude = magnitude::get(id);

                espurna::string key { "result:" };
                key += settings::keys::get(
                    magnitude, settings::suffix::Ratio).value();

                root.field(key, energy::ratioFromValue(magnitude, expected));
            });
        }
        return;
    }

    if (STRING_VIEW("emon-reset-ratios") == action) {
        energy::reset();
        return;
    }

    if (STRING_VIEW("magnitudes-pending") == action) {
        if (sensor::ready()) {
            wsPostSequence(client_id,
                {initial, list, settings});
        }
        return;
    }
}

void onVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, STRING_VIEW("sns"));
    for (auto sensor : internal::sensors) {
        if (isEmon(sensor)) {
            wsPayloadModule(root, STRING_VIEW("emon"));
        }

        if (isAnalog(sensor)) {
            wsPayloadModule(root, STRING_VIEW("analog"));
        }

        switch (sensor->id()) {
#if HLW8012_SUPPORT
        case SENSOR_HLW8012_ID:
            wsPayloadModule(root, STRING_VIEW("hlw"));
            break;
#endif
#if CSE7766_SUPPORT
        case SENSOR_CSE7766_ID:
            wsPayloadModule(root, STRING_VIEW("cse"));
            break;
#endif
#if PZEM004T_SUPPORT || PZEM004TV30_SUPPORT
        case SENSOR_PZEM004T_ID:
        case SENSOR_PZEM004TV30_ID:
            wsPayloadModule(root, STRING_VIEW("pzem"));
            break;
#endif
#if PULSEMETER_SUPPORT
        case SENSOR_PULSEMETER_ID:
            wsPayloadModule(root, STRING_VIEW("pm"));
            break;
#endif
        }
    }
}

void module(espurna::json::Writer& root, StringView prefix, SensorWebSocketMagnitudesCallback callback) {
    espurna::web::ws::EnumerablePayload payload{root, STRING_VIEW("magnitudes-module")};

    payload.root().field(STRING_VIEW("prefix"), prefix);

    payload(STRING_VIEW("values"), magnitude::count(),
        {{STRING_VIEW("type"), [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::get(index).type);
        }},
        {STRING_VIEW("index_global"), [](espurna::json::Writer& out, size_t index) {
            out.value(magnitude::get(index).index_global);
        }},
        {STRING_VIEW("index_module"), callback}
    });
}

void setup() {
    wsRegister()
        .onConnected(initial)
        .onConnected(list)
        .onConnected(settings)
        .onConnected(drivers)
        .onVisible(onVisible)
        .onData(onData)
        .onAction(onAction)
        .onKeyCheck(onKeyCheck);
}

} // namespace web
#endif

#if API_SUPPORT
namespace api {

template <typename T>
bool tryHandle(ApiRequest& request, unsigned char type, T&& callback) {
    size_t index = 0;
    if (request.wildcards()) {
        const auto param = request.wildcard(0);
        if (!::tryParseId(param, magnitude::types_count(type), index)) {
            return false;
        }
    }

    const auto* magnitude = magnitude::find(type, index);
    if (magnitude) {
        callback(*magnitude);
        return true;
    }

    return false;
}

void setup() {
    apiRegister("magnitudes",
        [](ApiRequest&, espurna::json::Writer& root) {
            root.array("magnitudes");
            for (auto& magnitude : magnitude::internal::magnitudes) {
                root.beginArray();
                root.value(sensor::magnitude::topicWithIndex(magnitude));
                root.value(magnitude.last.value);
                root.value(magnitude.reported.value);
                root.end();
            }
            root.end();
            return true;
        },
        nullptr
    );

    magnitude::forEachCounted([](unsigned char type) {
        auto pattern = magnitude::topic(type);
        if (sensor::build::useIndex() || (magnitude::types_count(type) > 1)) {
            pattern += STRING_VIEW("/+");
        }

        ApiBasicHandler get = [type](ApiRequest& request) {
            return tryHandle(request, type,
                [&](const Magnitude& magnitude) {
                    request.send(magnitude::format(magnitude,
                        magnitude::prefer_real_time_values() ? magnitude.last : magnitude.reported));
                    return true;
                });
        };

        ApiBasicHandler put;
        if (type == MAGNITUDE_ENERGY) {
            put = [](ApiRequest& request) {
                return tryHandle(request, MAGNITUDE_ENERGY,
                    [&](const Magnitude& magnitude) {
                        energy::set(magnitude, request.param("value"));
                    });
            };
        }

        apiRegister(std::move(pattern), std::move(get), std::move(put));
    });
}

} // namespace api
#endif

#if MQTT_SUPPORT
namespace mqtt {

void report(const Value& report, const Magnitude& magnitude [[gnu::unused]]) {
    mqttSend(report.topic.c_str(), report.repr.c_str());

#if SENSOR_PUBLISH_ADDRESSES
    STRING_VIEW_INLINE(AddressTopic, SENSOR_ADDRESS_TOPIC);

    const auto address = magnitude.sensor->address(magnitude.slot);
    if (address.length()) {
        espurna::string address_topic;
        address_topic.reserve(1 + report.topic.length() + AddressTopic.length());
        address_topic.concat(AddressTopic.data(), AddressTopic.length());
        address_topic += '/';
        address_topic += report.topic;

        mqttSend(address_topic.c_str(), address.c_str());
    }
#endif
}

void callback(unsigned int type, StringView topic, StringView payload) {
    if (!magnitude::types_count(MAGNITUDE_ENERGY)) {
        return;
    }

    static const auto base = magnitude::topic(MAGNITUDE_ENERGY);

    switch (type) {
    case MQTT_MESSAGE_EVENT:
    {
        auto t = mqttMagnitude(topic);
        if (!t.startsWith(base)) {
            break;
        }

        size_t index;
        if (!tryParseIdPath(t, magnitude::types_count(MAGNITUDE_ENERGY), index)) {
            break;
        }

        const auto* magnitude = magnitude::find(MAGNITUDE_ENERGY, index);
        if (magnitude) {
            energy::set(*magnitude, payload.toString());
        }

        break;
    }

    case MQTT_CONNECT_EVENT:
        mqttSubscribe((base + "/+").c_str());
        break;

    }
}

void setup() {
    ::mqttRegister(callback);
}

} // namespace mqtt
#endif

#if TERMINAL_SUPPORT
namespace terminal {
namespace commands {

PROGMEM_STRING(Sensors, "SENSORS");

void sensors(::terminal::CommandContext&& ctx) {
    if (!magnitude::count()) {
        terminalError(ctx, "No magnitudes");
        return;
    }

    size_t index = 0;
    for (const auto& magnitude : magnitude::internal::magnitudes) {
        ctx.output.printf_P(PSTR("%2zu * %s @ %s => %s\n"),
            index++,
            magnitude::topicWithIndex(magnitude).c_str(),
            magnitude::description(magnitude).c_str(),
            magnitude::format_slot(magnitude).c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Magnitudes, "MAGNITUDES");

void magnitudes(::terminal::CommandContext&& ctx) {
    if (!magnitude::count()) {
        terminalError(ctx, "No magnitudes");
        return;
    }

    size_t index = 0;
    for (const auto& magnitude : magnitude::internal::magnitudes) {
        ctx.output.printf_P(PSTR("%2zu * %s @ %s read %s reported %s\n"),
            index++,
            magnitude::topicWithIndex(magnitude).c_str(),
            magnitude::description(magnitude).c_str(),
            magnitude::format_with_units(magnitude, magnitude.last).c_str(),
            magnitude::format_with_units(magnitude, magnitude.reported).c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Expected, "EXPECTED");

void expected(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 3) {
        const auto id = espurna::settings::internal::convert<size_t>(ctx.argv[1]);
        if (id < magnitude::count()) {
            const auto& magnitude = magnitude::get(id);

            const auto result = energy::ratioFromValue(
                magnitude, espurna::settings::internal::convert<double>(ctx.argv[2]));
            const auto key = settings::keys::get(
                magnitude, settings::suffix::Ratio);
            ctx.output.printf("%s => %s\n", key.c_str(), espurna::string(result).c_str());
            terminalOK(ctx);
            return;
        }

        terminalError(ctx, "Invalid magnitude ID");
        return;
    }

    terminalError(ctx, "EXPECTED <ID> <VALUE>");
}

PROGMEM_STRING(ResetRatios, "RESET.RATIOS");

void reset_ratios(::terminal::CommandContext&& ctx) {
    energy::reset();
    terminalOK(ctx);
}

PROGMEM_STRING(Energy, "ENERGY");

void energy(::terminal::CommandContext&& ctx) {
    using IndexType = decltype(Magnitude::index_global);

    if (ctx.argv.size() < 2) {
        terminalError(ctx, "ENERGY <ID> [<VALUE>]");
        return;
    }

    const auto index = espurna::settings::internal::convert<IndexType>(ctx.argv[1]);

    const auto* magnitude = magnitude::find(MAGNITUDE_ENERGY, index);
    if (!magnitude) {
        terminalError(ctx, "Invalid magnitude ID");
        return;
    }

    if (ctx.argv.size() == 2) {
        ctx.output.printf_P(PSTR("%s => %s (%s)\n"),
            magnitude::topicWithIndex(*magnitude).c_str(),
            magnitude::format(*magnitude, magnitude->reported.value).c_str(),
            units::name(*magnitude).c_str());
        terminalOK(ctx);
        return;
    }

    if (ctx.argv.size() == 3) {
        const auto energy = energy::convert(ctx.argv[2]);
        if (!energy) {
            terminalError(ctx, "Invalid energy string");
            return;
        }

        energy::set(*magnitude, energy.value());
    }
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Sensors, commands::sensors},
    {Magnitudes, commands::magnitudes},
    {Expected, commands::expected},
    {ResetRatios, commands::reset_ratios},
    {Energy, commands::energy},
};

} // namespace commands

void setup() {
    espurna::terminal::add(commands::List);
}

} // namespace terminal
#endif

// -----------------------------------------------------------------------------
// Sensor initialization
// -----------------------------------------------------------------------------

namespace internal {

State state { State::None };
std::unique_ptr<ReadyFlag> init_flag;

ReadyFlag read_flag;

} // namespace internal

void configure_magnitude(Magnitude& magnitude) {
    // TODO: namespace and various helpers need some naming tweaks...

    // Only initialized once, notify about reset requirement?
    if (!magnitude.filter) {
        magnitude.filter_type = getSetting(
            settings::keys::get(magnitude, settings::suffix::Filter),
            magnitude::defaultFilter(magnitude));
        magnitude.filter = magnitude::makeFilter(magnitude.filter_type);
    }

    // Everything filtered so far is reset, possibly updating total number of required readings.
    magnitude.filter->resize(reportEvery());

    // Reset internal readings counter as well.
    magnitude.read_count = 0;

    // process emon-specific settings first. ensure that settings use global index and we access sensor with the local one
    if (isEmon(magnitude.sensor) && magnitude::traits::ratio_supported(magnitude.type)) {
        auto* sensor = static_cast<BaseEmonSensor*>(magnitude.sensor.get());
        sensor->setRatio(magnitude.slot, getSetting(
            settings::keys::get(magnitude, settings::suffix::Ratio),
            sensor->defaultRatio(magnitude.slot)));
    }

    // analog variant of emon sensor has some additional settings
    if (isAnalogEmon(magnitude.sensor) && (magnitude.type == MAGNITUDE_VOLTAGE)) {
        auto* sensor = static_cast<BaseAnalogEmonSensor*>(magnitude.sensor.get());
        sensor->setVoltage(getSetting(
            settings::keys::get(magnitude, settings::suffix::Mains),
            sensor->defaultVoltage()));
        sensor->setReferenceVoltage(getSetting(
            settings::keys::get(magnitude, settings::suffix::Reference),
            sensor->defaultReferenceVoltage()));
    }

    // adjust units based on magnitude's type
    magnitude.units = units::filter(magnitude,
        getSetting(
            settings::keys::get(magnitude, settings::suffix::Units),
            magnitude.sensor->units(magnitude.slot)));

    // adjust resulting value (simple plus or minus)
    // TODO: inject math or rpnlib expression?
    if (magnitude::traits::correction_supported(magnitude.type)) {
        magnitude.correction = getSetting(
            settings::keys::get(magnitude, settings::suffix::Correction),
            magnitude::build::correction(magnitude.type));
    }

    // pick decimal precision either from our (sane) defaults of from the sensor itself
    // (specifically, when sensor has more or less precision than we expect)
    {
        const auto decimals = magnitude.sensor->decimals(magnitude.units);
        magnitude.decimals = getSetting(
            settings::keys::get(magnitude, settings::suffix::Precision),
                (decimals >= 0)
                    ? static_cast<unsigned char>(decimals)
                    : magnitude::decimals(magnitude.units));
    }

    // Per-magnitude min & max delta of the report value, may trigger reports independent of the read counter overflow
    // - ${prefix}MinDelta${index} for value change greater than or equal to the specified delta
    // - ${prefix}MaxDelta${index} for value change less than or equal to the specified delta
    // Both are 0. by default, meaning these checks are ignored when processing read data
    magnitude.min_delta = getSetting(
        settings::keys::get(magnitude, settings::suffix::MinDelta),
        build::DefaultMinDelta);
    magnitude.max_delta = getSetting(
        settings::keys::get(magnitude, settings::suffix::MaxDelta),
        build::DefaultMaxDelta);

    // Overwrite value with 0 when below a certain threshold. Happens when report is triggered, before any further checks
    magnitude.zero_threshold = getSetting(
        settings::keys::get(magnitude, settings::suffix::ZeroThreshold),
        Value::Unknown);

    // Per-magnitude min & max checks of the report value
    magnitude.min_threshold = getSetting(
        settings::keys::get(magnitude, settings::suffix::MinThreshold),
        Value::Unknown);
    magnitude.max_threshold = getSetting(
        settings::keys::get(magnitude, settings::suffix::MaxThreshold),
        Value::Unknown);

    // When we don't save energy, purge existing value in both RAM & settings
    if (isEmon(magnitude.sensor) && (MAGNITUDE_ENERGY == magnitude.type) && (0 == energy::every())) {
        energy::reset(magnitude.index_global);
    }
}

// Update magnitude config, filter sizes and reset energy if needed
void configure_magnitudes() {
    for (auto& magnitude : magnitude::internal::magnitudes) {
        configure_magnitude(magnitude);
    }
}

void schedule_read() {
    internal::read_flag.stop_wait(internal::read_interval);
}

void suspend() {
    for (auto& sensor : internal::sensors) {
        sensor->suspend();
    }
}

void resume() {
    schedule_read();

    magnitude::forEachInstance(
        [](sensor::Magnitude& instance) {
            instance.read_count = 0;
            instance.filter->reset();
        });

    for (auto& sensor : internal::sensors) {
        sensor->resume();
    }
}

bool init() {
    bool out { true };

    while (!internal::pre_init.empty()) {
        auto it = internal::pre_init.begin();

        auto result = (*it)->find_sensors();
        if (result.error != SENSOR_ERROR_OK) {
            DEBUG_MSG_P(PSTR("[SENSOR] %s -> ERROR %s (%hhu)\n"),
                    (*it)->description().c_str(),
                    sensor::error(result.error).c_str(), result.error);
            out = false;
            break;
        }

        for (auto* ptr : result.sensors) {
            sensor::add(ptr);
        }

        internal::pre_init.pop_front();
    }

    for (auto sensor : internal::sensors) {
        // Do not process an already initialized sensor
        if (sensor->ready()) {
            continue;
        }

        // Force sensor to reload config
        sensor->begin();

        if (!sensor->ready()) {
            const auto error = sensor->error();
            if (error != SENSOR_ERROR_OK) {
                DEBUG_MSG_P(PSTR("[SENSOR] %s -> ERROR %s (%hhu)\n"),
                    sensor->description().c_str(),
                    sensor::error(error).c_str(), error);
            }
            out = false;
            break;
        }

        const auto slots = (unsigned char){ sensor->count() };
        for (auto slot = (unsigned char){ 0 }; slot < slots; ++slot) {
            auto& result = magnitude::add(sensor, sensor->type(slot), slot);
            configure_magnitude(result);

            // Energy tracking is implemented by looking at the specific magnitude & it's index at read time
            if (MAGNITUDE_ENERGY == result.type) {
                energy::setup(result);
            }

            // Track both supported and currently used units
            units::setup(result.type);
        }
    }

    if (out) {
        internal::state = State::Ready;

        if (sensor::count()) {
            DEBUG_MSG_P(PSTR("[SENSOR] Finished initialization for %zu sensor(s) and %zu magnitude(s)\n"),
                sensor::count(), magnitude::count());
        } else {
            DEBUG_MSG_P(PSTR("[SENSOR] Finished initialization\n"));
        }
    }

    return out;
}

// setup() helper. try init() and schedule re-initialization in loop()
void try_init() {
    if (!init()) {
        using T = decltype(internal::init_flag)::element_type;
        auto flag = std::make_unique<T>();
        flag->wait(internal::init_interval);
        internal::init_flag = std::move(flag);
    }
}

// loop() helper. if init flag was set previously, re-try init() until it works
bool maybe_try_init(duration::Milliseconds interval) {
    if (!internal::init_flag) {
        return true;
    }

    if (internal::init_flag->wait(interval) && init()) {
        internal::init_flag.reset(nullptr);
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Magnitude processing
// -----------------------------------------------------------------------------

void notify() {
    decltype(notifications::internal::callbacks) callbacks;
    std::swap(callbacks, notifications::internal::callbacks);

    for (auto sensor : internal::sensors) {
        for (auto& callback : callbacks) {
            if (callback(sensor.get())) {
                sensor->notify();
            }
        }
    }

    notifications::internal::timers.remove_if(
        [](const timer::SystemTimer& timer) {
            return !static_cast<bool>(timer);
        });
}

void tick() {
    for (auto sensor : internal::sensors) {
        sensor->tick();
    }
}

void pre() {
    for (auto sensor : internal::sensors) {
        sensor->pre();
    }
}

void post() {
    for (auto sensor : internal::sensors) {
        sensor->post();
    }
}

void error() {
#if DEBUG_SUPPORT
    for (auto sensor : internal::sensors) {
        if (SENSOR_ERROR_OK != sensor->error()) {
            DEBUG_MSG_P(PSTR("[SENSOR] Could not read from %s - %s\n"),
                    sensor->description().c_str(),
                    error(sensor->error()).c_str());
        }
    }
#endif
}

void reset_report(duration::Seconds read_interval, size_t report_every) {
    internal::read_interval = read_interval;
    internal::report_every = report_every;
    internal::read_flag.stop_wait(read_interval);
}

bool ready_to_read() {
    return internal::read_flag.wait(internal::read_interval);
}

bool ready_to_report(ValuePair& out, const ValuePair& processed, const Magnitude& magnitude, bool report) {
    // Ensure that reported value change is greater or equal to this delta value
    const bool compare_min_delta { magnitude.min_delta > build::DefaultMinDelta };
    report = report || compare_min_delta;

    // Ensure that reported value change is less or equal to this delta value
    const bool compare_max_delta { magnitude.max_delta > build::DefaultMaxDelta };
    report = report || compare_max_delta;

    // Ensure that reported value is greater than or equal to this value
    const bool check_min_threshold { !std::isnan(magnitude.min_threshold) };
    report = report || check_min_threshold;

    // Ensure that reported value is less than or equal to this value
    const bool check_max_threshold { !std::isnan(magnitude.max_threshold) };
    report = report || check_max_threshold;


    if (report) {
        if (magnitude.filter->ready()) {
            out = ValuePair{
                .value = magnitude.filter->value(),
                .units = processed.units,
            };
            magnitude.filter->restart();
        } else {
            out = processed;
        }

        // Figure out whether report value should be zero or not
        if (!std::isnan(magnitude.zero_threshold) && out.value < magnitude.zero_threshold) {
            out.value = 0.0;
        }

        const bool previous_report { !std::isnan(magnitude.reported.value) };

        if (report && previous_report && compare_min_delta) {
            report = std::abs(out.value - magnitude.reported.value)
                >= magnitude.min_delta;
        }

        if (report && previous_report && compare_max_delta) {
            report = std::abs(out.value - magnitude.reported.value)
                <= magnitude.max_delta;
        }

        if (report && check_min_threshold) {
            report = out.value >= magnitude.min_threshold;
        }

        if (report && check_max_threshold) {
            report = out.value <= magnitude.max_threshold;
        }
    }

    return report;
}

void loop() {
    // TODO: allow to do nothing
    if (internal::state == State::Idle) {
        return;
    }

    // Continiously repeat initialization if there are still some un-initialized sensors after setup()
    if (internal::state == State::None) {
        internal::state = State::Initial;
    }

    // General initialization, generate magnitudes from available sensors
    if (internal::state == State::Initial) {
        if (maybe_try_init(settings::initInterval())) {
            internal::state = State::Ready;
        }
    }

    // If magnitudes were initialized and we are ready, prepare to read sensor data
    if (internal::state == State::Ready) {
        if (magnitude::internal::magnitudes.size() != 0) {
            schedule_read();
            internal::state = State::Reading;
        }
    }

    if (internal::state != State::Reading) {
        return;
    }

    // Notify hook, called when requested by sensor
    sensor::notify();

    // Tick hook, called every loop()
    sensor::tick();

    if (ready_to_read()) {
        // XXX: Filter out certain magnitude types when relay is turned OFF
#if RELAY_SUPPORT && SENSOR_POWER_CHECK_STATUS
        const bool relay_on = (relayCount() == 1) && (relayStatus(0) == RelayStatus::On);
#endif

        // Report every Nth reading
        const auto report_every = reportEvery();

        // Pre-read hook, called every reading
        sensor::pre();

        // Notify about sensor errors that may have been updated by pre()
        sensor::error();

        // Current magnitude reading state
        struct {
            ValuePair raw;       // as the sensor returns it
            ValuePair processed; // after applying units and decimals
            ValuePair report;    // value to be reported (either processed, or filtered)
        } state;

        for (size_t index = 0; index < magnitude::count(); ++index) {
            auto& magnitude = magnitude::get(index);

            // Do not read anything from a failed sensor
            if (SENSOR_ERROR_OK != magnitude.sensor->error()) {
                continue;
            }

            // Value from the sensor as-is
            state.raw = ValuePair{
                .value = magnitude.sensor->value(magnitude.slot),
                .units = magnitude.sensor->units(magnitude.slot),
            };

            // Completely remove spurious values if relay is OFF
#if RELAY_SUPPORT && SENSOR_POWER_CHECK_STATUS
            switch (magnitude.type) {
            case MAGNITUDE_POWER_ACTIVE:
            case MAGNITUDE_POWER_REACTIVE:
            case MAGNITUDE_POWER_APPARENT:
            case MAGNITUDE_POWER_FACTOR:
            case MAGNITUDE_CURRENT:
            case MAGNITUDE_ENERGY_DELTA:
                if (!relay_on) {
                    state.raw.value = 0.0;
                }
                break;
            default:
                break;
            }
#endif

            // Apply units and correct number of decimals (directly modifies the double value)
            state.processed = magnitude::process(magnitude, state.raw);

            // Absolute value correction. *Unconditional*, value is always offset by this amount
            state.processed.value += magnitude.correction;

            // In case units change occurred, make sure filter receives the same unit type
            if (magnitude.last.units != state.processed.units) {
                magnitude.filter->reset();
            }

            magnitude.filter->update(state.processed.value);

            // Making last reading available in API and for external listeners
            magnitude.last = state.processed;
            magnitude::read(magnitude::value(magnitude, state.processed));

            // At this point, we should decide whether this value should be reported.
            // First, increment read counter and check for overflow.
            const auto read_count = magnitude.read_count;
            magnitude.read_count = (read_count + 1) % report_every;

            bool report { 0 == magnitude.read_count };

            // Special case for energy, save current readings to
            // - RTC memory (always)
            // - Internal flash (optionally, when reporting)
            if (MAGNITUDE_ENERGY == magnitude.type) {
                energy::update(magnitude, report);
            }

            // Prepare and verify report value before proceeding
            report = ready_to_report(
                state.report, state.processed,
                magnitude, report);

            // If flag was not reset by the checks above, continue and finally report the value
            if (report) {
                const auto value = magnitude::value(magnitude, state.report);

                magnitude.reported = state.report;
                magnitude::report(value);

#if MQTT_SUPPORT
                mqtt::report(value, magnitude);
#endif
#if THINGSPEAK_SUPPORT
                tspkEnqueueMagnitude(index, value.repr);
#endif
#if DOMOTICZ_SUPPORT
                domoticzSendMagnitude(index, value);
#endif
            }

#if SENSOR_DEBUG
            {
                DEBUG_MSG_P(PSTR("[SENSOR] %s -> raw %s processed %s report %s\n"),
                    magnitude::topic(magnitude).c_str(),
                    magnitude::format_with_units(magnitude, state.raw).c_str(),
                    magnitude::format_with_units(magnitude, state.processed).c_str(),
                    magnitude::format_with_units(magnitude, state.report).c_str());
            }
#endif
        }

        sensor::post();

#if WEB_SUPPORT
        wsPost(web::onData);
#endif
    }
}

void configure_base() {
    // Read counter is set for each magnitude, and equals to 0 right after this point
    reset_report(
        sensor::settings::readInterval(),
        sensor::settings::reportEvery());

    // Generic 'get magnitude value' API calls prefer latest values over the reported ones
    magnitude::prefer_real_time_values(sensor::settings::realTimeValues());

    // TODO: something more generic? energy is an accumulating value, only allow for similar ones?
    // TODO: move to an external module?
    energy::every(sensor::settings::saveEvery());
}

void configure() {
    configure_base();
    configure_magnitudes();
}

void setup() {
    // Make sure settings stay up-to-date
    migrateVersion(settings::migrate);

    // Initialize what is there: the drivers come later, as their modules start (sensorDriver())
    sensor::try_init();

    // Minimal configuration required before entering loop()
    sensor::configure_base();

    // Allow us to query key default
    sensor::settings::query::setup();

    // Websockets integration, send sensor readings and configuration
#if WEB_SUPPORT
    web::setup();
#endif

    // Publishes sensor reports, and {re,}set energy
#if MQTT_SUPPORT
    mqtt::setup();
#endif

#if API_SUPPORT
    api::setup();
#endif

#if TERMINAL_SUPPORT
    terminal::setup();
#endif

    systemBeforeSleep(sensor::suspend);
    systemAfterSleep(sensor::resume);

    espurnaRegisterLoop(sensor::loop);
    espurnaRegisterReload(sensor::configure);
}

} // namespace

bool ready() {
    return (State::Reading == internal::state) || (State::Ready == internal::state && magnitude::count() == 0);
}

void add_preinit_impl(PreInitPtr ptr) {
    internal::pre_init.push_front(std::move(ptr));
}

} // namespace sensor
} // namespace espurna

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------

#if WEB_SUPPORT
// Used by modules to generate magnitude_id<->module_id mapping for the WebUI
// Prefix controls the UI templates, supplied callback should retrieve module-specific value Id
void sensorWebSocketMagnitudes(espurna::json::Writer& root, espurna::StringView prefix, SensorWebSocketMagnitudesCallback callback) {
    espurna::sensor::web::module(root, prefix, callback);
}
#endif // WEB_SUPPORT

void sensorOnMagnitudeRead(MagnitudeReadHandler handler) {
    espurna::sensor::magnitude::onRead(handler);
}

void sensorOnMagnitudeReport(MagnitudeReadHandler handler) {
    espurna::sensor::magnitude::onReport(handler);
}

size_t magnitudeCount() {
    return espurna::sensor::magnitude::count();
}

unsigned char magnitudeIndex(unsigned char index) {
    using namespace espurna::sensor;

    if (index < magnitude::count()) {
        return magnitude::get(index).index_global;
    }

    return 0;
}

unsigned char magnitudeType(unsigned char index) {
    using namespace espurna::sensor;

    if (index < magnitude::count()) {
        return magnitude::get(index).type;
    }

    return MAGNITUDE_NONE;
}

espurna::sensor::Value magnitudeReadValue(unsigned char index) {
    return espurna::sensor::magnitude::safe_value_last(index);
}

espurna::sensor::Value magnitudeReportValue(unsigned char index) {
    return espurna::sensor::magnitude::safe_value_reported(index);
}

espurna::sensor::Value magnitudeValue(unsigned char index) {
    using namespace espurna::sensor::magnitude;
    return prefer_real_time_values()
        ? safe_value_last(index)
        : safe_value_reported(index);
}

espurna::string magnitudeDescription(unsigned char index) {
    using namespace espurna::sensor;

    if (index < magnitude::count()) {
        return magnitude::description(magnitude::get(index));
    }

    return espurna::string();
}

espurna::string magnitudeTopic(unsigned char index) {
    using namespace espurna::sensor;

    if (index < magnitude::count()) {
        return magnitude::topicWithIndex(magnitude::get(index));
    }

    return espurna::string();
}

espurna::string magnitudeTypeTopic(unsigned char type) {
    return espurna::sensor::magnitude::topic(type);
}

espurna::string magnitudeUnitsName(espurna::sensor::Unit units) {
    return espurna::sensor::units::name(units);
}

espurna::sensor::Info magnitudeInfo(unsigned char index) {
    using namespace espurna::sensor;

    if (index < magnitude::count()) {
        return magnitude::info(magnitude::get(index));
    }

    return Info {
        .type = MAGNITUDE_NONE,
        .index = 0,
        .units = Unit::None,
        .decimals = 0,
        .topic = espurna::string(),
        .description = espurna::string(),
    };
}

bool sensorReady() {
    return espurna::sensor::ready();
}

espurna::string sensorList() {
    espurna::string out;
    for (const auto& driver : espurna::sensor::internal::drivers) {
        if (out.length()) {
            out += ' ';
        }
        out += driver.name;
    }

    return out;
}

// --- what the driver modules call (through the head: modules_api.cpp, the provides of the sensors module)

void sensorDriver(const espurna::sensor::Driver& driver) {
    using namespace espurna::sensor;

    internal::drivers.push_back(driver);
    DEBUG_MSG_P(PSTR("[SENSOR] Driver %s\n"), driver.name);

    // what the settings of the driver say is there, then begin() what came: the same as the load of the sensors
    // used to be in setup(), once per driver now. A sensor that does not begin yet is retried from the loop
    driver.load();
    internal::state = State::None;
    try_init();
}

void sensorAdd(BaseSensor* sensor) {
    espurna::sensor::add(sensor);
}

void sensorAddPreinit(espurna::sensor::PreInit* ptr) {
    espurna::sensor::add_preinit_impl(espurna::sensor::PreInitPtr(ptr));
}

void sensorNotifyAfter(uint32_t milliseconds, espurna::sensor::NotifyCallback callback) {
    espurna::sensor::notify_after_impl(espurna::duration::Milliseconds(milliseconds), callback);
}

void sensorNotifyNow(espurna::sensor::NotifyCallback callback) {
    espurna::sensor::notify_now_impl(callback);
}

void sensorSetup() {
    espurna::sensor::setup();
}

#endif // SENSOR_SUPPORT

