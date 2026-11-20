/*

PROTOTYPE: MQTT as a module (docs/modules.md)

The head of the ESP8266 image is built with MQTT_MODULE=1: relays, buttons, LEDs, the terminal ... call the MQTT API as
always, but the head has only forwarders (modules_api.cpp) to the table the mqtt module hands over when it
starts (module_mqtt.cpp). Without the module: not connected, nothing sent. Callbacks registered before it starts (the
head's own parts register in their setup) are kept by the head and given to the module when it comes.

*/

#pragma once

#include "mqtt.h"

struct MqttModuleApi {
    void (*heartbeat)(espurna::heartbeat::Callback);
    void (*callback)(MqttCallback);
    void (*onPublish)(uint16_t, MqttPidCallback);
    void (*onSubscribe)(uint16_t, MqttPidCallback);

    espurna::string (*topic)(const espurna::string&);
    espurna::string (*topicIndex)(const espurna::string&, size_t);
    espurna::string (*setter)(const espurna::string&);
    espurna::string (*setterIndex)(const espurna::string&, size_t);
    espurna::StringView (*magnitude)(espurna::StringView);

    uint16_t (*sendRaw4)(const char*, const char*, bool, int);
    uint16_t (*sendRaw5)(const char*, const char*, bool, int, MqttPidCallback);
    uint16_t (*sendRaw3)(const char*, const char*, bool);
    uint16_t (*sendRaw2)(const char*, const char*);

    uint16_t (*subscribeRaw2)(const char*, int);
    uint16_t (*subscribeRaw1)(const char*);
    bool (*subscribe)(const char*);
    uint16_t (*unsubscribeRaw)(const char*);
    bool (*unsubscribe)(const char*);

    bool (*send4)(const char*, const char*, bool, bool);
    bool (*send3)(const char*, const char*, bool);
    bool (*send2)(const char*, const char*);
    bool (*sendIndex5)(const char*, unsigned int, const char*, bool, bool);
    bool (*sendIndex4)(const char*, unsigned int, const char*, bool);
    bool (*sendIndex3)(const char*, unsigned int, const char*);

    void (*sendStatus)();
    void (*flush)();
    void (*enqueue)(espurna::StringView, espurna::StringView);

    const espurna::string& (*payloadOnline)();
    const espurna::string& (*payloadOffline)();
    const char* (*payloadStatus)(bool);

    void (*setEnabled)(bool);
    bool (*enabled)();
    bool (*forward)();
    bool (*connected)();
    void (*disconnect)();
};

// the module's entry: its table; the head gives it the callbacks registered so far
void modulesMqttProvide(const MqttModuleApi* api);
