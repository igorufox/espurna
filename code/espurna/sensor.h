/*

SENSOR MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once


#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ratio>

#include "json.h"

#include "types.h"

class BaseSensor;

namespace espurna {
namespace sensor {

enum class Unit : int {
    Min_,
    None,
    Celcius,
    Farenheit,
    Kelvin,
    Percentage,
    Hectopascal,
    Ampere,
    Volt,
    Voltampere,
    Kilovoltampere,
    VoltampereReactive,
    KilovoltampereReactive,
    Watt,
    Kilowatt,
    WattSecond,
    Joule = WattSecond,
    KilowattHour,
    PartsPerMillion,
    Ohm,
    MicrogrammPerCubicMeter,   // The concentration of an air pollutant
    MilligrammPerCubicMeter,   //
    Lux,
    UltravioletIndex,          // "measurement of the strength of sunburn-producing ultraviolet (UV) radiation at a particular place and time"
                               // (XXX: Not a unit. Distinguish from None and specify decimals)
    CountsPerMinute,           // Unit of local dose rate (Geiger counting)
    MicrosievertPerHour,       // 2nd unit of local dose rate (Geiger counting)
    Meter,
    Hertz,
    Ph,
    Max_ = 32
};

enum class Filter : int {
    Last,
    Min,
    Max,
    Median,
    MovingAverage,
    Sum,
};

struct Watts {
    using Type = double;
    using Ratio = std::ratio<1>;
    Type value;
};

struct WattSeconds {
    using Type = uint32_t;
    using Ratio = std::ratio_multiply<
        Watts::Ratio,
        espurna::duration::Seconds::period>;
    Type value { 0 };

    WattSeconds() = default;

    constexpr explicit WattSeconds(Type value) :
        value(value)
    {}

    constexpr explicit WattSeconds(float value) :
        value(static_cast<Type>(value))
    {}

    constexpr explicit WattSeconds(double value) :
        value(static_cast<Type>(value))
    {}

    constexpr WattSeconds(Watts watts, espurna::duration::Seconds seconds) :
        value(static_cast<Type>(watts.value * seconds.count()))
    {}
};

struct WattHours {
    using Type = uint32_t;
    using Ratio = std::ratio_multiply<
        Watts::Ratio,
        espurna::duration::Hours::period>;
    Type value { 0 };

    WattHours() = default;
    explicit WattHours(Type value) :
        value(value)
    {}

    WattHours(Watts watts, espurna::duration::Hours hours) :
        value(static_cast<Type>(watts.value * hours.count()))
    {}
};

struct Kilowatts {
    using Type = double;
    using Ratio = std::ratio<1000>;
    Type value;
};

struct KilowattHours {
    using Type = uint32_t;
    using Ratio = std::ratio_multiply<
        Kilowatts::Ratio,
        espurna::duration::Hours::period>;
    Type value { 0 };

    KilowattHours() = default;
    explicit KilowattHours(Type value) :
        value(value)
    {}

    KilowattHours(Kilowatts kilowatts, espurna::duration::Hours hours) :
        value(static_cast<Type>(kilowatts.value * hours.count()))
    {}
};

template <typename To, typename From,
          typename Divide = std::ratio_divide<typename From::Ratio, typename To::Ratio>>
struct Convert {
    static To from(From from) {
        return To(from.value
                * static_cast<typename To::Type>(Divide::num)
                / static_cast<typename To::Type>(Divide::den));
    }
};

struct Energy {
    struct Pair {
        KilowattHours kwh;
        WattSeconds ws;
    };

    static constexpr auto WattSecondsMax =
        WattSeconds::Type(KilowattHours::Ratio::num);

    Energy() = default;
    Energy(const Energy&) = default;
    Energy(Energy&&) = default;

    // energy always consists of kwh + ws pair
    explicit Energy(Pair);

    // prefer integral types
    explicit Energy(WattSeconds);
    explicit Energy(WattHours);
    explicit Energy(KilowattHours kwh) :
        _kwh(kwh)
    {}

    // special case for kwh input
    explicit Energy(double);

    Energy& operator=(const Energy&) = default;
    Energy& operator=(Energy&&) = default;

    // sets internal counters to zero
    void reset();

    // check whether we have *any* energy recorded. Can be zero:
    // - on cold boot
    // - on overflow
    // - when we call `reset()`
    explicit operator bool() const;

    // allow generic math operation when dealing with energy delta
    Energy& operator+=(const Energy&);

    // most sensor implementations handle energy in joules / watt-second
    Energy& operator+=(WattSeconds);
    Energy operator+(WattSeconds);

    // numeric representation as kWh
    double asDouble() const;

    // API representation as `<kWh>+<Ws>`
    espurna::string asString() const;

    // represent internal value as watt seconds / joules
    // **will rollover** when exceeding WattSeconds::Type capacity
    // (when kWh value is greater or equal to `maximum of Type / Ws per kWh`)
    WattSeconds asWattSeconds() const;

    // we are storing a kind-of integral and fractional parts
    // using watt-second to avoid loosing precision, we don't expect these to be accessed directly
    Pair pair() const {
        return Pair {
            .kwh = _kwh,
            .ws = _ws,
        };
    }

private:
    KilowattHours _kwh;
    WattSeconds _ws;
};

// '.value' is set to 'Value::Unknown' when index is out of bounds
// '.value' is undefined when either reading or report hadn't happened yet
struct Value {
    static constexpr double Unknown {
        std::numeric_limits<double>::quiet_NaN() };

    unsigned char type;
    unsigned char index;

    espurna::string slot;

    Unit units;
    unsigned char decimals;
    espurna::string topic;

    double value;
    espurna::string repr;

    explicit operator bool() const;
};

struct Info {
    unsigned char type;
    unsigned char index;

    Unit units;
    unsigned char decimals;

    espurna::string topic;
    espurna::string description;
};

using NotifyCallback = bool (*)(const BaseSensor*);

// A sensor driver: a module of its own (sensors/module_<name>.cpp, config/head.h), started after the sensors module.
// Its entry registers it with sensorDriver(); the sensors module calls load() right away, load() reads the settings of
// the driver and adds the sensors they configure with sensorAdd() (none when its pins / port / address are not set).
// The SENSORS panel of the WebUI shows the settings of every registered driver, from `spec`:
//   fields separated by ';', a field "key|type|label|default|options|repeat"
//   type    gpio, adc (a pin of the ADC), uart (1-based number of a port of the uart module, 0 - off), i2c (an
//           address, 0x.. hex), number, text, bool, select
//   options select: "value=Text,value=Text", number: "min-max"
//   repeat  the key and the label have '#' for the index: keys 0 .. repeat - 1, labels 1 .. repeat
// The changes apply at the next boot (the drivers load once, when they start)
struct Driver {
    const char* name;   // the module name
    const char* spec;   // PROGMEM
    void (*load)();
};

struct PreInit {
    using Sensors = Span<BaseSensor*>;

    struct Result {
        Sensors sensors;
        int error;
    };

    // inline: the sensor drivers (separate modules) derive from it
    virtual ~PreInit() = default;

    virtual Result find_sensors() = 0;
    virtual espurna::string description() const = 0;
};

bool ready();

using PreInitPtr = std::unique_ptr<PreInit>;

} // namespace sensor
} // namespace espurna

// what the driver modules call: the sensors module's, a driver is linked against it directly (it needs it)
void sensorDriver(const espurna::sensor::Driver&);
void sensorAdd(BaseSensor*);
void sensorAddPreinit(espurna::sensor::PreInit*);
void sensorNotifyAfter(uint32_t milliseconds, espurna::sensor::NotifyCallback);
void sensorNotifyNow(espurna::sensor::NotifyCallback);

namespace espurna {
namespace sensor {

inline void notify_after(duration::Milliseconds after, NotifyCallback callback) {
    sensorNotifyAfter(after.count(), callback);
}

inline void notify_now(NotifyCallback callback) {
    sensorNotifyNow(callback);
}

inline void add_preinit(PreInitPtr ptr) {
    sensorAddPreinit(ptr.release());
}

} // namespace sensor
} // namespace espurna

//--------------------------------------------------------------------------------

espurna::string magnitudeTypeTopic(unsigned char type);
espurna::string magnitudeUnitsName(espurna::sensor::Unit);

using MagnitudeReadHandler = void(*)(const espurna::sensor::Value&);

// Executes 'handler(value)' every time sensor reading happens
// (depends on read interval and won't happen in case sensor returns an error)
void sensorOnMagnitudeRead(MagnitudeReadHandler handler);

// Executes 'handler(value)' every time sensor report happens
// (depends on report counter of sensor reads and conditions like min / max delta)
void sensorOnMagnitudeReport(MagnitudeReadHandler handler);

// Amount of registered sensor magnitudes aka measurements
size_t magnitudeCount();

// Base magnitude info. Will contain `.type = MAGNITUDE_NONE` when index is out of bounds
espurna::sensor::Info magnitudeInfo(unsigned char index);

// type of magnitude at index; returns MAGNITUDE_NONE when index is out of bounds
unsigned char magnitudeType(unsigned char index);

// returns global index of magnitudes of the same type
unsigned char magnitudeIndex(unsigned char index);

espurna::string magnitudeTopic(unsigned char index);

// Get either last or reported reading; repends on the real-time setting
espurna::sensor::Value magnitudeValue(unsigned char index);

// Retrieves last sensor reading of the magnitude at index
espurna::sensor::Value magnitudeReadValue(unsigned char index);

// Retrieves last reported value of the magnitude at index
espurna::sensor::Value magnitudeReportValue(unsigned char index);

using SensorWebSocketMagnitudesCallback = void(*)(espurna::json::Writer&, size_t);
void sensorWebSocketMagnitudes(espurna::json::Writer& root, espurna::StringView prefix, SensorWebSocketMagnitudesCallback);

bool sensorReady();

// the names of the registered drivers
espurna::string sensorList();
void sensorSetup();

// --- the energy counter, inline: the energy meter drivers are modules of their own, apart from the sensors module

#include <cmath>
#include <limits>

namespace espurna {
namespace sensor {

// Generic storage. Most of the time we init this on boot with both members or start at 0 and increment with watt-second

inline Energy::Energy(Energy::Pair pair) :
    _kwh(pair.kwh),
    _ws(pair.ws)
{}

inline Energy::Energy(WattSeconds ws) {
    _ws.value = ws.value;
    while (_ws.value >= WattSecondsMax) {
        _ws.value -= WattSecondsMax;
        ++_kwh.value;
    }
}

inline Energy::Energy(WattHours other) :
    Energy(static_cast<double>(other.value) / 1000.0)
{}

inline Energy::Energy(double kwh) {
    double lhs;
    double rhs = std::modf(kwh, &lhs);

    _kwh.value = lhs;
    _ws.value = rhs * static_cast<double>(KilowattHours::Ratio::num);
}

inline Energy& Energy::operator+=(WattSeconds other) {
    return *this += Energy(other);
}

inline Energy Energy::operator+(WattSeconds other) {
    Energy result(*this);
    result += other;

    return result;
}

inline Energy& Energy::operator+=(const Energy& other) {
    _kwh.value += other._kwh.value;

    const auto left = WattSecondsMax - _ws.value;
    if (other._ws.value >= left) {
        _kwh.value += 1;
        _ws.value = (other._ws.value - left);
    } else {
        _ws.value += other._ws.value;
    }

    return *this;
}

inline Energy::operator bool() const {
    return (_kwh.value > 0) && (_ws.value > 0);
}

inline WattSeconds Energy::asWattSeconds() const {
    using Type = WattSeconds::Type;

    static constexpr auto TypeMax = std::numeric_limits<Type>::max();
    static constexpr Type KwhMax { TypeMax / WattSecondsMax };

    auto kwh = _kwh.value;
    while (kwh >= KwhMax) {
        kwh -= KwhMax;
    }

    WattSeconds out;
    out.value += _ws.value;
    out.value += kwh * WattSecondsMax;

    return out;
}

inline double Energy::asDouble() const {
    return static_cast<double>(_kwh.value)
        + static_cast<double>(_ws.value)
        / static_cast<double>(WattSecondsMax);
}

inline espurna::string Energy::asString() const {
    espurna::string out;

    // Value without `+` is treated as just `<kWh>`
    out += espurna::string(_kwh.value, 10);
    if (_ws.value) {
        out += '+';
        out += espurna::string(_ws.value, 10);
    }

    return out;
}

inline void Energy::reset() {
    *this = Energy{};
}

} // namespace sensor
} // namespace espurna
