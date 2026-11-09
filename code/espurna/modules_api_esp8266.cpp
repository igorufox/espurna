/*

PROTOTYPE: functions the modules of the 1 MB head image provide to each other (docs/esp8266-modules.md, modules.h)

The head has none of these: here they forward to the slot the provider's entry filled, and answer "nothing" when the
provider is not installed (no magnitudes, no Domoticz). The provider's own definitions are renamed in its module
(scripts/esp8266_module_link.py, `provides` in platformio_modules.ini), so every other module ends up here.

*/

#include "espurna.h"

#if MODULES_SUPPORT

#include "modules.h"
#include "sensor.h"

using espurna::modules::Api;

extern "C" {
void* espurna_module_api[static_cast<size_t>(Api::Count)] {};
}

namespace {

template <typename T>
T* api(Api slot) {
    return modulesApi<T>(slot);
}

} // namespace

// --- sensors module

size_t magnitudeCount() {
    const auto func = api<decltype(magnitudeCount)>(Api::MagnitudeCount);
    return func ? func() : 0;
}

espurna::sensor::Info magnitudeInfo(unsigned char index) {
    const auto func = api<decltype(magnitudeInfo)>(Api::MagnitudeInfo);
    if (func) {
        return func(index);
    }

    espurna::sensor::Info out{};
    out.type = MAGNITUDE_NONE;
    return out;
}

String magnitudeTypeTopic(unsigned char type) {
    const auto func = api<decltype(magnitudeTypeTopic)>(Api::MagnitudeTypeTopic);
    return func ? func(type) : String();
}

String magnitudeUnitsName(espurna::sensor::Unit units) {
    const auto func = api<decltype(magnitudeUnitsName)>(Api::MagnitudeUnitsName);
    return func ? func(units) : String();
}

bool sensorReady() {
    const auto func = api<decltype(sensorReady)>(Api::SensorReady);
    return func ? func() : false;
}

void sensorWebSocketMagnitudes(JsonObject& root, espurna::StringView prefix, SensorWebSocketMagnitudesCallback callback) {
    const auto func = api<decltype(sensorWebSocketMagnitudes)>(Api::SensorWebSocketMagnitudes);
    if (func) {
        func(root, prefix, callback);
    }
}

void sensorOnMagnitudeRead(MagnitudeReadHandler handler) {
    const auto func = api<decltype(sensorOnMagnitudeRead)>(Api::SensorOnMagnitudeRead);
    if (func) {
        func(handler);
    }
}

void sensorOnMagnitudeReport(MagnitudeReadHandler handler) {
    const auto func = api<decltype(sensorOnMagnitudeReport)>(Api::SensorOnMagnitudeReport);
    if (func) {
        func(handler);
    }
}

namespace {

espurna::sensor::Value noValue() {
    espurna::sensor::Value out{};
    out.type = MAGNITUDE_NONE;
    return out;
}

} // namespace

espurna::sensor::Value magnitudeValue(unsigned char index) {
    const auto func = api<decltype(magnitudeValue)>(Api::MagnitudeValue);
    return func ? func(index) : noValue();
}

espurna::sensor::Value magnitudeReportValue(unsigned char index) {
    const auto func = api<decltype(magnitudeReportValue)>(Api::MagnitudeReportValue);
    return func ? func(index) : noValue();
}

espurna::sensor::Value magnitudeReadValue(unsigned char index) {
    const auto func = api<decltype(magnitudeReadValue)>(Api::MagnitudeReadValue);
    return func ? func(index) : noValue();
}

unsigned char magnitudeType(unsigned char index) {
    const auto func = api<decltype(magnitudeType)>(Api::MagnitudeType);
    return func ? func(index) : MAGNITUDE_NONE;
}

unsigned char magnitudeIndex(unsigned char index) {
    const auto func = api<decltype(magnitudeIndex)>(Api::MagnitudeIndex);
    return func ? func(index) : 0;
}

String magnitudeTopic(unsigned char index) {
    const auto func = api<decltype(magnitudeTopic)>(Api::MagnitudeTopic);
    return func ? func(index) : String();
}

// --- Domoticz and Thingspeak modules, called by the sensors module

void domoticzSendMagnitude(unsigned char index, const espurna::sensor::Value& value) {
    const auto func = api<void(unsigned char, const espurna::sensor::Value&)>(Api::DomoticzSendMagnitude);
    if (func) {
        func(index, value);
    }
}

bool tspkEnqueueMagnitude(unsigned char index, const String& value) {
    const auto func = api<bool(unsigned char, const String&)>(Api::TspkEnqueueMagnitude);
    return func ? func(index, value) : false;
}


// --- light module, called by Home Assistant, Alexa, Domoticz, the scheduler

#include "light.h"

void lightState(bool state) {
    const auto func = api<void(bool)>(Api::LightStateSet);
    if (func) {
        func(state);
    }
}

void lightState(size_t id, bool state) {
    const auto func = api<void(size_t, bool)>(Api::LightStateIdSet);
    if (func) {
        func(id, state);
    }
}

bool lightState() {
    const auto func = api<bool()>(Api::LightStateGet);
    return func ? func() : false;
}

void lightUpdate() {
    const auto func = api<void()>(Api::LightUpdate);
    if (func) {
        func();
    }
}

void lightUpdate(LightTransition transition) {
    const auto func = api<void(LightTransition)>(Api::LightUpdateTransition);
    if (func) {
        func(transition);
    }
}

long lightChannel(size_t id) {
    const auto func = api<long(size_t)>(Api::LightChannelGet);
    return func ? func(id) : 0;
}

void lightChannel(size_t id, long value) {
    const auto func = api<void(size_t, long)>(Api::LightChannelSet);
    if (func) {
        func(id, value);
    }
}

size_t lightChannels() {
    const auto func = api<size_t()>(Api::LightChannels);
    return func ? func() : 0;
}

void lightOnReport(LightReportListener listener) {
    const auto func = api<void(LightReportListener)>(Api::LightOnReport);
    if (func) {
        func(listener);
    }
}

String lightColor() {
    const auto func = api<String()>(Api::LightColor);
    return func ? func() : String();
}

bool lightUseRGB() {
    const auto func = api<bool()>(Api::LightUseRgb);
    return func ? func() : false;
}

bool lightHasColor() {
    const auto func = api<bool()>(Api::LightHasColor);
    return func ? func() : false;
}

bool lightHasWhite() {
    const auto func = api<bool()>(Api::LightHasWhite);
    return func ? func() : false;
}

bool lightHasColdWhite() {
    const auto func = api<bool()>(Api::LightHasColdWhite);
    return func ? func() : false;
}

bool lightHasWarmWhite() {
    const auto func = api<bool()>(Api::LightHasWarmWhite);
    return func ? func() : false;
}

void lightColdWhite(long value) {
    const auto func = api<void(long)>(Api::LightColdWhiteSet);
    if (func) {
        func(value);
    }
}

long lightColdWhite() {
    const auto func = api<long()>(Api::LightColdWhiteGet);
    return func ? func() : 0;
}

void lightWarmWhite(long value) {
    const auto func = api<void(long)>(Api::LightWarmWhiteSet);
    if (func) {
        func(value);
    }
}

long lightWarmWhite() {
    const auto func = api<long()>(Api::LightWarmWhiteGet);
    return func ? func() : 0;
}

void lightBrightness(long value) {
    const auto func = api<void(long)>(Api::LightBrightnessSet);
    if (func) {
        func(value);
    }
}

long lightBrightness() {
    const auto func = api<long()>(Api::LightBrightnessGet);
    return func ? func() : 0;
}

void lightBrightnessPercent(long value) {
    const auto func = api<void(long)>(Api::LightBrightnessPercent);
    if (func) {
        func(value);
    }
}

espurna::light::TemperatureRange lightMiredsRange() {
    const auto func = api<espurna::light::TemperatureRange()>(Api::LightMiredsRange);
    return func ? func() : espurna::light::TemperatureRange{153, 500};
}

void lightTemperature(espurna::light::Mireds value) {
    const auto func = api<void(espurna::light::Mireds)>(Api::LightTemperatureMireds);
    if (func) {
        func(value);
    }
}

espurna::duration::Milliseconds lightTransitionStep() {
    const auto func = api<espurna::duration::Milliseconds()>(Api::LightTransitionStep);
    return func ? func() : espurna::duration::Milliseconds{0};
}

espurna::duration::Milliseconds lightTransitionTime() {
    const auto func = api<espurna::duration::Milliseconds()>(Api::LightTransitionTime);
    return func ? func() : espurna::duration::Milliseconds{0};
}

void lightHs(long hue, long saturation) {
    const auto func = api<void(long, long)>(Api::LightHs);
    if (func) {
        func(hue, saturation);
    }
}

espurna::light::Hsv lightHsv() {
    const auto func = api<espurna::light::Hsv()>(Api::LightHsvGet);
    return func ? func() : espurna::light::Hsv{};
}

void lightRgb(espurna::light::Rgb value) {
    const auto func = api<void(espurna::light::Rgb)>(Api::LightRgbSet);
    if (func) {
        func(value);
    }
}

espurna::light::Rgb lightRgb() {
    const auto func = api<espurna::light::Rgb()>(Api::LightRgbGet);
    return func ? func() : espurna::light::Rgb{};
}

void lightRed(long value) {
    const auto func = api<void(long)>(Api::LightRedSet);
    if (func) {
        func(value);
    }
}

void lightGreen(long value) {
    const auto func = api<void(long)>(Api::LightGreenSet);
    if (func) {
        func(value);
    }
}

void lightBlue(long value) {
    const auto func = api<void(long)>(Api::LightBlueSet);
    if (func) {
        func(value);
    }
}

#endif // MODULES_SUPPORT

#if MODULES_SUPPORT

// ArduinoOTA references mDNS although ESPurna starts it without (begin(false)): these stubs take its references
// (scripts/esp8266_head_exports.py links the head with --wrap), the real mDNS is in the mdns module
extern "C" {

alignas(4) char __wrap_MDNS[8];

bool espurna_mdns_stub_begin(void*, const char*, const void*, uint32_t)
    __asm__("__wrap__ZN7esp826618MDNSImplementation13MDNSResponder5beginEPKcRK9IPAddressj");
bool espurna_mdns_stub_begin(void*, const char*, const void*, uint32_t) {
    return false;
}

bool espurna_mdns_stub_enable(void*, uint16_t, bool)
    __asm__("__wrap__ZN7esp826618MDNSImplementation13MDNSResponder13enableArduinoEtb");
bool espurna_mdns_stub_enable(void*, uint16_t, bool) {
    return false;
}

bool espurna_mdns_stub_end(void*)
    __asm__("__wrap__ZN7esp826618MDNSImplementation13MDNSResponder3endEv");
bool espurna_mdns_stub_end(void*) {
    return false;
}

bool espurna_mdns_stub_update(void*)
    __asm__("__wrap__ZN7esp826618MDNSImplementation13MDNSResponder6updateEv");
bool espurna_mdns_stub_update(void*) {
    return false;
}

} // extern "C"

#endif // MODULES_SUPPORT
