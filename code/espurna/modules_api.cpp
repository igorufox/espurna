/*

PROTOTYPE: functions the modules provide to each other (docs/modules.md, modules.h)

The head has none of these: here they forward to the slot the provider's entry filled, and answer "nothing" when the
provider is not installed (no magnitudes, no Domoticz). The provider's own definitions are renamed in its module
(scripts/module_link.py, the provides column of ESPURNA_MODULES in config/head.h), so every other module ends up
here. The same on every chip.

*/

#include "espurna.h"

#if MODULES_SUPPORT

#include "modules.h"
#include "sensor.h"
#include "relay.h"
#include "button.h"
#include "api.h"
#include "ntp.h"

using espurna::modules::Api;

// espurna_module_api[] itself is in modules.cpp (every chip)

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

// --- relay module: the modules that follow the relays and work without them too (button, led, sensors, scheduler,
// alexa, Home Assistant, Domoticz, Thingspeak, rf, light, Matter, NSPanel): no relays when it is not installed

size_t relayCount() {
    const auto func = api<decltype(relayCount)>(Api::RelayCount);
    return func ? func() : 0;
}

RelayStatus relayStatus(size_t id) {
    const auto func = api<RelayStatus(size_t)>(Api::RelayStatusGet);
    return func ? func(id) : RelayStatus::NotAvailable;
}

// any relay on (the led module)
bool relayStatus() {
    const auto func = api<bool()>(Api::RelayStatusAll);
    return func ? func() : false;
}

bool relayStatus(size_t id, bool status) {
    const auto func = api<bool(size_t, bool)>(Api::RelayStatusSet);
    return func ? func(id, status) : false;
}

bool relayToggle(size_t id) {
    const auto func = api<decltype(relayToggle)>(Api::RelayToggle);
    return func ? func(id) : false;
}

PayloadStatus relayParsePayload(espurna::StringView payload) {
    const auto func = api<decltype(relayParsePayload)>(Api::RelayParsePayload);
    return func ? func(payload) : PayloadStatus::Unknown;
}

espurna::StringView relayPayload(PayloadStatus status) {
    const auto func = api<decltype(relayPayload)>(Api::RelayPayload);
    return func ? func(status) : espurna::StringView();
}

void relayOnStatusChange(RelayStatusCallback callback) {
    const auto func = api<decltype(relayOnStatusChange)>(Api::RelayOnStatusChange);
    if (func) {
        func(callback);
    }
}

void relayOnStatusNotify(RelayStatusCallback callback) {
    const auto func = api<decltype(relayOnStatusNotify)>(Api::RelayOnStatusNotify);
    if (func) {
        func(callback);
    }
}

void relayOnReady(RelayStatusCallback callback) {
    const auto func = api<decltype(relayOnReady)>(Api::RelayOnReady);
    if (func) {
        func(callback);
    }
}

int relayBtnGpio(size_t index) {
    const auto func = api<decltype(relayBtnGpio)>(Api::RelayBtnGpio);
    return func ? func(index) : -1;
}

// --- button module (NSPanel: its own action of the buttons)

void buttonSetCustomAction(ButtonEventHandler handler) {
    const auto func = api<decltype(buttonSetCustomAction)>(Api::ButtonSetCustomAction);
    if (func) {
        func(handler);
    }
}

// --- api module: the modules that register API paths work without it

void apiRegister(String path, espurna::api::BasicHandler&& get, espurna::api::BasicHandler&& put) {
    const auto func = api<void(String, espurna::api::BasicHandler&&, espurna::api::BasicHandler&&)>(
        Api::ApiRegisterBasic);
    if (func) {
        func(std::move(path), std::move(get), std::move(put));
    }
}

void apiRegister(String path, espurna::api::JsonHandler&& get, espurna::api::JsonHandler&& put) {
    const auto func = api<void(String, espurna::api::JsonHandler&&, espurna::api::JsonHandler&&)>(
        Api::ApiRegisterJson);
    if (func) {
        func(std::move(path), std::move(get), std::move(put));
    }
}

// --- ntp module: the time for the head (the log, the pages) and the modules; no time without it

void ntpOnTick(NtpTickCallback callback) {
    const auto func = api<decltype(ntpOnTick)>(Api::NtpOnTick);
    if (func) {
        func(callback);
    }
}

String ntpDateTime() {
    const auto func = api<decltype(ntpDateTime)>(Api::NtpDateTime);
    return func ? func() : String();
}

bool ntpSynced() {
    const auto func = api<decltype(ntpSynced)>(Api::NtpSynced);
    return func ? func() : false;
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

// --- Alexa module, called by the Matter module (its quiet mode while a controller commissions)

void alexaPause(bool paused) {
    const auto func = api<void(bool)>(Api::AlexaPause);
    if (func) {
        func(paused);
    }
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

// the button module steps the brightness; the relay module takes the light's state as a relay (a relay provider)
void lightBrightnessStep(long steps) {
    const auto func = api<void(long)>(Api::LightBrightnessStep);
    if (func) {
        func(steps);
    }
}

void lightBrightnessStep(long steps, long multiplier) {
    const auto func = api<void(long, long)>(Api::LightBrightnessStepMultiplier);
    if (func) {
        func(steps, multiplier);
    }
}

std::unique_ptr<RelayProviderBase> lightMakeStateRelayProvider(size_t id) {
    const auto func = api<decltype(lightMakeStateRelayProvider)>(Api::LightMakeStateRelayProvider);
    return func ? func(id) : nullptr;
}

#endif // MODULES_SUPPORT

#if MODULES_SUPPORT

// ArduinoOTA references mDNS although ESPurna starts it without (begin(false)): these stubs take its references
// (scripts/module_head.py, custom_head_wraps links the head with --wrap), the real mDNS is in the mdns module
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

// --- MQTT as a module (modules_mqtt.h): the head's MQTT API forwards to the mqtt module's table ----------------------

#if MODULES_SUPPORT && MQTT_SUPPORT && MQTT_MODULE

#include "modules_mqtt.h"

namespace {

const MqttModuleApi* mqtt_api { nullptr };

// registered before the module started (the head's parts in their setup): handed over when it comes
MqttCallback mqtt_callbacks[24] {};
size_t mqtt_callbacks_count { 0 };
espurna::heartbeat::Callback mqtt_heartbeats[8] {};
size_t mqtt_heartbeats_count { 0 };

const String& mqttEmpty() {
    static const String out;
    return out;
}

} // namespace

void modulesMqttProvide(const MqttModuleApi* api) {
    mqtt_api = api;
    if (!api) {
        return;
    }
    for (size_t index = 0; index < mqtt_callbacks_count; ++index) {
        api->callback(mqtt_callbacks[index]);
    }
    for (size_t index = 0; index < mqtt_heartbeats_count; ++index) {
        api->heartbeat(mqtt_heartbeats[index]);
    }
}

void mqttHeartbeat(espurna::heartbeat::Callback callback) {
    if (mqtt_api) {
        mqtt_api->heartbeat(callback);
    } else if (mqtt_heartbeats_count < std::size(mqtt_heartbeats)) {
        mqtt_heartbeats[mqtt_heartbeats_count++] = callback;
    }
}

void mqttRegister(MqttCallback callback) {
    if (mqtt_api) {
        mqtt_api->callback(callback);
    } else if (mqtt_callbacks_count < std::size(mqtt_callbacks)) {
        mqtt_callbacks[mqtt_callbacks_count++] = callback;
    }
}

void mqttOnPublish(uint16_t pid, MqttPidCallback callback) {
    if (mqtt_api) {
        mqtt_api->onPublish(pid, std::move(callback));
    }
}

void mqttOnSubscribe(uint16_t pid, MqttPidCallback callback) {
    if (mqtt_api) {
        mqtt_api->onSubscribe(pid, std::move(callback));
    }
}

String mqttTopic(const String& magnitude) {
    return mqtt_api ? mqtt_api->topic(magnitude) : String();
}

String mqttTopic(const String& magnitude, size_t index) {
    return mqtt_api ? mqtt_api->topicIndex(magnitude, index) : String();
}

String mqttTopicSetter(const String& magnitude) {
    return mqtt_api ? mqtt_api->setter(magnitude) : String();
}

String mqttTopicSetter(const String& magnitude, size_t index) {
    return mqtt_api ? mqtt_api->setterIndex(magnitude, index) : String();
}

espurna::StringView mqttMagnitude(espurna::StringView topic) {
    return mqtt_api ? mqtt_api->magnitude(topic) : espurna::StringView();
}

uint16_t mqttSendRaw(const char* topic, const char* message, bool retain, int qos) {
    return mqtt_api ? mqtt_api->sendRaw4(topic, message, retain, qos) : 0;
}

uint16_t mqttSendRaw(const char* topic, const char* message, bool retain, int qos, MqttPidCallback on_publish) {
    return mqtt_api ? mqtt_api->sendRaw5(topic, message, retain, qos, std::move(on_publish)) : 0;
}

uint16_t mqttSendRaw(const char* topic, const char* message, bool retain) {
    return mqtt_api ? mqtt_api->sendRaw3(topic, message, retain) : 0;
}

uint16_t mqttSendRaw(const char* topic, const char* message) {
    return mqtt_api ? mqtt_api->sendRaw2(topic, message) : 0;
}

uint16_t mqttSubscribeRaw(const char* topic, int qos) {
    return mqtt_api ? mqtt_api->subscribeRaw2(topic, qos) : 0;
}

uint16_t mqttSubscribeRaw(const char* topic) {
    return mqtt_api ? mqtt_api->subscribeRaw1(topic) : 0;
}

bool mqttSubscribe(const char* topic) {
    return mqtt_api ? mqtt_api->subscribe(topic) : false;
}

uint16_t mqttUnsubscribeRaw(const char* topic) {
    return mqtt_api ? mqtt_api->unsubscribeRaw(topic) : 0;
}

bool mqttUnsubscribe(const char* topic) {
    return mqtt_api ? mqtt_api->unsubscribe(topic) : false;
}

bool mqttSend(const char* topic, const char* message, bool force, bool retain) {
    return mqtt_api ? mqtt_api->send4(topic, message, force, retain) : false;
}

bool mqttSend(const char* topic, const char* message, bool force) {
    return mqtt_api ? mqtt_api->send3(topic, message, force) : false;
}

bool mqttSend(const char* topic, const char* message) {
    return mqtt_api ? mqtt_api->send2(topic, message) : false;
}

bool mqttSend(const char* topic, unsigned int index, const char* message, bool force, bool retain) {
    return mqtt_api ? mqtt_api->sendIndex5(topic, index, message, force, retain) : false;
}

bool mqttSend(const char* topic, unsigned int index, const char* message, bool force) {
    return mqtt_api ? mqtt_api->sendIndex4(topic, index, message, force) : false;
}

bool mqttSend(const char* topic, unsigned int index, const char* message) {
    return mqtt_api ? mqtt_api->sendIndex3(topic, index, message) : false;
}

void mqttSendStatus() {
    if (mqtt_api) {
        mqtt_api->sendStatus();
    }
}

void mqttFlush() {
    if (mqtt_api) {
        mqtt_api->flush();
    }
}

void mqttEnqueue(espurna::StringView topic, espurna::StringView payload) {
    if (mqtt_api) {
        mqtt_api->enqueue(topic, payload);
    }
}

const String& mqttPayloadOnline() {
    return mqtt_api ? mqtt_api->payloadOnline() : mqttEmpty();
}

const String& mqttPayloadOffline() {
    return mqtt_api ? mqtt_api->payloadOffline() : mqttEmpty();
}

const char* mqttPayloadStatus(bool status) {
    return mqtt_api ? mqtt_api->payloadStatus(status) : (status ? "1" : "0");
}

void mqttEnabled(bool status) {
    if (mqtt_api) {
        mqtt_api->setEnabled(status);
    }
}

bool mqttEnabled() {
    return mqtt_api ? mqtt_api->enabled() : false;
}

bool mqttForward() {
    return mqtt_api ? mqtt_api->forward() : false;
}

bool mqttConnected() {
    return mqtt_api ? mqtt_api->connected() : false;
}

void mqttDisconnect() {
    if (mqtt_api) {
        mqtt_api->disconnect();
    }
}

// the module's entry runs the real one
void mqttSetup() {
}

#endif // MODULES_SUPPORT && MQTT_SUPPORT && MQTT_MODULE

// --- the mdns module: mDNS for the other modules (MQTT finds its broker with it); off - not running, no answers -----

#if MODULES_SUPPORT && !MDNS_SERVER_SUPPORT

#include "espurna_mdns.h"

bool mdnsRunning() {
    const auto func = modulesApi<decltype(mdnsRunning)>(espurna::modules::Api::MdnsRunning);
    return func ? func() : false;
}

bool mdnsServiceQuery(const String& service, const String& protocol, MdnsServerQueryCallback callback) {
    const auto func = modulesApi<decltype(mdnsServiceQuery)>(espurna::modules::Api::MdnsServiceQuery);
    return func ? func(service, protocol, callback) : false;
}

#endif // MODULES_SUPPORT && !MDNS_SERVER_SUPPORT

// --- uart module: the ports. Before it starts (or without it) there is no port: the serial log, the relays of a UART
// board and the drivers find none and try again later

#if MODULES_SUPPORT && UART_MODULE

#include "uart.h"

espurna::driver::uart::PortPtr uartPort(size_t index) {
    const auto func = modulesApi<decltype(uartPort)>(espurna::modules::Api::UartPort);
    return func ? func(index) : nullptr;
}

bool uartClaim(size_t index, const __FlashStringHelper* owner) {
    const auto func = modulesApi<decltype(uartClaim)>(espurna::modules::Api::UartClaim);
    return func ? func(index, owner) : false;
}

const __FlashStringHelper* uartClaimed(size_t index) {
    const auto func = modulesApi<decltype(uartClaimed)>(espurna::modules::Api::UartClaimed);
    return func ? func(index) : nullptr;
}

void uartOnClaim(UartClaimRelease callback) {
    const auto func = modulesApi<decltype(uartOnClaim)>(espurna::modules::Api::UartOnClaim);
    if (func) {
        func(callback);
    }
}

espurna::driver::uart::PortPtr uartOpen(size_t index) {
    const auto func = modulesApi<decltype(uartOpen)>(espurna::modules::Api::UartOpen);
    return func ? func(index) : nullptr;
}

bool uartBaud(size_t index, uint32_t value) {
    const auto func = modulesApi<decltype(uartBaud)>(espurna::modules::Api::UartBaud);
    return func ? func(index, value) : false;
}

#endif // MODULES_SUPPORT && UART_MODULE
