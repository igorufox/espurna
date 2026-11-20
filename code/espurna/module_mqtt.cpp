/*

PROTOTYPE: the MQTT module (docs/modules.md, modules_mqtt.h), env:esp8266-1m-modules of platformio_esp8266.ini

mqtt.cpp linked against the head: its client, settings, WebUI panel and terminal commands. It hands its API over to
the head (the head's relays, buttons, LEDs ... and the other modules call the head's forwarders), the head gives it
the callbacks registered before it started. Home Assistant and Domoticz need it.

*/

#include "espurna.h"

#if MODULE_BUILD && MQTT_SUPPORT

#include "modules.h"
#include "modules_mqtt.h"

namespace {

const MqttModuleApi Table {
    .heartbeat = mqttHeartbeat,
    .callback = mqttRegister,
    .onPublish = mqttOnPublish,
    .onSubscribe = mqttOnSubscribe,

    .topic = static_cast<espurna::string(*)(const espurna::string&)>(mqttTopic),
    .topicIndex = static_cast<espurna::string(*)(const espurna::string&, size_t)>(mqttTopic),
    .setter = static_cast<espurna::string(*)(const espurna::string&)>(mqttTopicSetter),
    .setterIndex = static_cast<espurna::string(*)(const espurna::string&, size_t)>(mqttTopicSetter),
    .magnitude = mqttMagnitude,

    .sendRaw4 = static_cast<uint16_t(*)(const char*, const char*, bool, int)>(mqttSendRaw),
    .sendRaw5 = static_cast<uint16_t(*)(const char*, const char*, bool, int, MqttPidCallback)>(mqttSendRaw),
    .sendRaw3 = static_cast<uint16_t(*)(const char*, const char*, bool)>(mqttSendRaw),
    .sendRaw2 = static_cast<uint16_t(*)(const char*, const char*)>(mqttSendRaw),

    .subscribeRaw2 = static_cast<uint16_t(*)(const char*, int)>(mqttSubscribeRaw),
    .subscribeRaw1 = static_cast<uint16_t(*)(const char*)>(mqttSubscribeRaw),
    .subscribe = mqttSubscribe,
    .unsubscribeRaw = mqttUnsubscribeRaw,
    .unsubscribe = mqttUnsubscribe,

    .send4 = static_cast<bool(*)(const char*, const char*, bool, bool)>(mqttSend),
    .send3 = static_cast<bool(*)(const char*, const char*, bool)>(mqttSend),
    .send2 = static_cast<bool(*)(const char*, const char*)>(mqttSend),
    .sendIndex5 = static_cast<bool(*)(const char*, unsigned int, const char*, bool, bool)>(mqttSend),
    .sendIndex4 = static_cast<bool(*)(const char*, unsigned int, const char*, bool)>(mqttSend),
    .sendIndex3 = static_cast<bool(*)(const char*, unsigned int, const char*)>(mqttSend),

    .sendStatus = mqttSendStatus,
    .flush = mqttFlush,
    .enqueue = mqttEnqueue,

    .payloadOnline = mqttPayloadOnline,
    .payloadOffline = mqttPayloadOffline,
    .payloadStatus = mqttPayloadStatus,

    .setEnabled = static_cast<void(*)(bool)>(mqttEnabled),
    .enabled = static_cast<bool(*)()>(mqttEnabled),
    .forward = mqttForward,
    .connected = mqttConnected,
    .disconnect = mqttDisconnect,
};

} // namespace

// before the modules that need it (Home Assistant, Domoticz)
#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_mqtt.ipp"
#endif

ESPURNA_MODULE(mqtt, "mqtt", 5) {
#if WEB_SUPPORT
    modulesUi("mqtt", ui_mqtt_data, sizeof(ui_mqtt_data));
#endif

    mqttSetup();
    modulesMqttProvide(&Table);
}

#endif
