/*

INFLUXDB MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

The POST to /write goes through the head's one HTTP client (libs/http_client.h), driven by the loop.

*/

#include "espurna.h"

#if INFLUXDB_SUPPORT

#include <map>
#include <memory>

#include "influxdb.h"
#include "mqtt.h"
#include "relay.h"
#include "rpc.h"
#include "sensor.h"
#include "terminal.h"
#include "ws.h"

#include "libs/http_client.h"

namespace {

constexpr size_t DataBufferSize { 256 };

bool _idb_enabled = false;
bool _idb_flush = false;
std::map<String, String> _idb_values;
String _idb_payload;
String _idb_url;

espurna::http::Client _idb_client;
espurna::http::Discard _idb_sink;

} // namespace

// -----------------------------------------------------------------------------

STRING_VIEW_INLINE(IdbPrefix, "idb");

bool _idbWebSocketOnKeyCheck(espurna::StringView key, const JsonVariant& value) {
    return key.startsWith(IdbPrefix);
}

void _idbWebSocketOnVisible(JsonObject& root) {
    wsPayloadModule(root, IdbPrefix);
}

void _idbWebSocketOnConnected(JsonObject& root) {
    root["idbEnabled"] = getSetting("idbEnabled", 1 == INFLUXDB_ENABLED);
    root["idbHost"] = getSetting("idbHost", INFLUXDB_HOST);
    root["idbPort"] = getSetting("idbPort", INFLUXDB_PORT);
    root["idbDatabase"] = getSetting("idbDatabase", INFLUXDB_DATABASE);
    root["idbUsername"] = getSetting("idbUsername", INFLUXDB_USERNAME);
    root["idbPassword"] = getSetting("idbPassword", INFLUXDB_PASSWORD);
}

void _idbConfigure() {
    _idb_enabled = getSetting("idbEnabled", 1 == INFLUXDB_ENABLED);
    if (_idb_enabled && (getSetting("idbHost", INFLUXDB_HOST).length() == 0)) {
        _idb_enabled = false;
        setSetting("idbEnabled", 0);
    }
}

void _idbSendSensor(const espurna::sensor::Value& value) {
    idbSend(magnitudeTypeTopic(value.type).c_str(), value.index, value.repr.c_str());
}

void _idbSendStatus(size_t id, bool status) {
    idbSend(MQTT_TOPIC_RELAY, id, status ? "1" : "0"); // "status" ?
}

// -----------------------------------------------------------------------------

bool idbSend(const char * topic, const char * payload) {
    if (!_idb_enabled) return false;
    if (_idb_client.busy()) return false;

    _idb_values[topic] = payload;
    _idb_flush = true;

    return true;
}

// ref: https://docs.influxdata.com/influxdb/v1.7/tools/api/#summary-table-1 (204 - written)
void _idbDone(const espurna::http::Result& result) {
    if (result.ok) {
        DEBUG_MSG_P(PSTR("[INFLUXDB] %s, HTTP %d\n"), (result.status == 204) ? "Success" : "Failure", result.status);
    } else {
        DEBUG_MSG_P(PSTR("[INFLUXDB] Failure: %s\n"), result.message);
    }

    _idb_payload = "";
    _idb_url = "";
}

void _idbSend(const String& host, const uint16_t port) {
    DEBUG_MSG_P(PSTR("[INFLUXDB] Sending to %s:%u\n"), host.c_str(), port);

    _idb_url = F("http://");
    _idb_url += host;
    _idb_url += ':';
    _idb_url += String(port, 10);
    _idb_url += F("/write?db=");
    _idb_url += getSetting("idbDatabase", INFLUXDB_DATABASE);
    _idb_url += F("&u=");
    _idb_url += getSetting("idbUsername", INFLUXDB_USERNAME);
    _idb_url += F("&p=");
    _idb_url += getSetting("idbPassword", INFLUXDB_PASSWORD);

    espurna::http::Request request;
    request.url = _idb_url.c_str();
    request.method = "POST";
    request.type = "text/plain";
    request.body = reinterpret_cast<const uint8_t*>(_idb_payload.c_str());
    request.body_len = _idb_payload.length();
    _idb_client.start(request, _idb_sink, _idbDone);
}

void _idbFlush() {
    _idb_client.loop();

    // Wait until current connection is finished
    if (!_idb_enabled) return;
    if (!_idb_flush) return;
    if (_idb_client.busy()) return;

    // Wait until connected
    if (!wifiConnected()) return;

    _idb_flush = false;

    const auto host = getSetting("idbHost", INFLUXDB_HOST);
    const auto port = getSetting("idbPort", static_cast<uint16_t>(INFLUXDB_PORT));

    // TODO: should we always store specific pairs like tspk keeps relay / sensor readings?
    //       note that we also send heartbeat data, persistent values should be flagged
    const String device = systemHostname();

    _idb_payload = "";
    _idb_payload.reserve(DataBufferSize);
    for (auto& pair : _idb_values) {
        if (!isNumber(pair.second)) {
            String quoted;
            quoted.reserve(pair.second.length() + 2);
            quoted += '"';
            quoted += pair.second;
            quoted += '"';
            pair.second = quoted;
        }

        char buffer[128] = {0};
        snprintf_P(buffer, sizeof(buffer),
            PSTR("%s,device=%s value=%s\n"),
            pair.first.c_str(), device.c_str(), pair.second.c_str()
        );
        _idb_payload += buffer;
    }
    _idb_values.clear();

    _idbSend(host, port);
}

bool idbSend(const char * topic, unsigned char id, const char * payload) {
    char measurement[64];
    snprintf(measurement, sizeof(measurement), "%s,id=%d", topic, id);
    return idbSend(measurement, payload);
}

bool idbEnabled() {
    return _idb_enabled;
}

bool _idbHeartbeat(espurna::heartbeat::Mask mask) {
    if (mask & espurna::heartbeat::Report::Uptime)
        idbSend(MQTT_TOPIC_UPTIME, String(systemUptime().count()).c_str());

    if (mask & espurna::heartbeat::Report::Freeheap) {
        auto stats = systemHeapStats();
        idbSend(MQTT_TOPIC_FREEHEAP, String(stats.available).c_str());
    }

    if (mask & espurna::heartbeat::Report::Rssi)
        idbSend(MQTT_TOPIC_RSSI, String(WiFi.RSSI()).c_str());

    if ((mask & espurna::heartbeat::Report::Vcc) && (ADC_MODE_VALUE == ADC_VCC))
        idbSend(MQTT_TOPIC_VCC, String(ESP.getVcc()).c_str());

    if (mask & espurna::heartbeat::Report::Loadavg)
        idbSend(MQTT_TOPIC_LOADAVG, String(systemLoadAverage()).c_str());

    if (mask & espurna::heartbeat::Report::Ssid)
        idbSend(MQTT_TOPIC_SSID, WiFi.SSID().c_str());

    if (mask & espurna::heartbeat::Report::Bssid)
        idbSend(MQTT_TOPIC_BSSID, WiFi.BSSIDstr().c_str());

    return true;
}

#if TERMINAL_SUPPORT
PROGMEM_STRING(IdbSend, "IDB.SEND");

static void idbTerminalSend(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 4) {
        terminalError(ctx, F("idb.send <topic> <id> <value>"));
        return;
    }

    idbSend(ctx.argv[1].c_str(), ctx.argv[2].toInt(), ctx.argv[3].c_str());
}

static constexpr ::terminal::Command IdbCommands[] {
    {IdbSend, idbTerminalSend},
};

static void idbTerminalSetup() {
    espurna::terminal::add(IdbCommands);
}
#endif

void idbSetup() {
    systemHeartbeat(_idbHeartbeat);
    systemHeartbeat(_idbHeartbeat,
        getSetting("idbHbMode", espurna::heartbeat::currentMode()),
        getSetting("idbHbIntvl", espurna::heartbeat::currentInterval()));

    _idbConfigure();

    #if WEB_SUPPORT
        wsRegister()
            .onVisible(_idbWebSocketOnVisible)
            .onConnected(_idbWebSocketOnConnected)
            .onKeyCheck(_idbWebSocketOnKeyCheck);
    #endif

    #if RELAY_SUPPORT
        relayOnStatusChange(_idbSendStatus);
    #endif

    #if SENSOR_SUPPORT
        sensorOnMagnitudeReport(_idbSendSensor);
    #endif

    espurnaRegisterReload(_idbConfigure);
    espurnaRegisterLoop(_idbFlush);

    #if TERMINAL_SUPPORT
        idbTerminalSetup();
    #endif
}

#endif
