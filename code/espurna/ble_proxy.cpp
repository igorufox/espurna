/*

BLE PROXY: the advertisements the board hears, raw, to MQTT (owner 2026-11-19: «Сырые пакеты в MQTT»), the ble_proxy
module. The server decodes them (any sensor, any tag, presence by MAC).

Topic <root>/ble/<MAC> (MAC without colons, as printed: AABBCCDDEEFF), payload
    {"rssi":-71,"type":0,"data":"0201061aff4c00..."}
type: 0 public, 1 random address; data: the AD structures of the advertising PDU (or the scan response), hex.

The same bytes of the same device go out again no sooner than bleProxyInterval seconds (default 10), changed bytes no
sooner than 1 s: a beacon advertising every 100 ms does not flood the broker. bleProxyRssi (default -100): devices
heard weaker than this are left out. The devices it keeps track of: the last 32, the oldest gives its place.

*/

#include "espurna.h"

#if MODULE_BUILD

#include "ble.h"
#include "mqtt.h"

#include <cstring>

namespace espurna {
namespace ble {
namespace proxy {
namespace {

namespace build {

constexpr size_t DevicesMax { 32 };

constexpr uint32_t interval() {
    return 10;
}

constexpr int rssi() {
    return -100;
}

constexpr auto Changed = espurna::duration::Seconds(1);

} // namespace build

struct Device {
    uint8_t mac[6];
    uint32_t crc;
    espurna::time::CoreClock::time_point last;
};

Device devices[build::DevicesMax];
size_t devices_used { 0 };

espurna::duration::Seconds interval { build::interval() };
int min_rssi { build::rssi() };

uint32_t crc(const uint8_t* data, size_t length) {
    // FNV-1a: only to tell the same bytes from changed ones
    uint32_t out = 2166136261u;
    for (size_t index = 0; index < length; ++index) {
        out = (out ^ data[index]) * 16777619u;
    }
    return out;
}

// the record of the device, a new one in place of the oldest; nullptr - the same bytes too soon
Device* due(const Advert& advert, uint32_t sum, espurna::time::CoreClock::time_point now) {
    for (size_t index = 0; index < devices_used; ++index) {
        auto& device = devices[index];
        if (std::memcmp(device.mac, advert.mac, sizeof(device.mac)) != 0) {
            continue;
        }

        const auto age = now - device.last;
        if ((device.crc == sum) ? (age < interval) : (age < build::Changed)) {
            return nullptr;
        }

        return &device;
    }

    Device* device = nullptr;
    if (devices_used < build::DevicesMax) {
        device = &devices[devices_used++];
    } else {
        device = &devices[0];
        for (size_t index = 1; index < devices_used; ++index) {
            if (devices[index].last < device->last) {
                device = &devices[index];
            }
        }
    }

    std::memcpy(device->mac, advert.mac, sizeof(device->mac));
    return device;
}

void on_advert(const Advert& advert) {
    if ((advert.rssi < min_rssi) || !mqttConnected()) {
        return;
    }

    const auto now = espurna::time::millis();
    const uint32_t sum = crc(advert.data, advert.length);
    auto* device = due(advert, sum, now);
    if (!device) {
        return;
    }

    device->crc = sum;
    device->last = now;

    char topic[24];
    std::snprintf(topic, sizeof(topic), "ble/%02X%02X%02X%02X%02X%02X",
        advert.mac[0], advert.mac[1], advert.mac[2], advert.mac[3], advert.mac[4], advert.mac[5]);

    // {"rssi":-100,"type":1,"data":"<62 hex>"}
    char payload[96];
    int length = std::snprintf(payload, sizeof(payload), "{\"rssi\":%d,\"type\":%u,\"data\":\"",
        advert.rssi, advert.addr_type);
    static constexpr char Hex[] = "0123456789abcdef";
    for (size_t index = 0; (index < advert.length) && ((length + 4) < static_cast<int>(sizeof(payload))); ++index) {
        payload[length++] = Hex[advert.data[index] >> 4];
        payload[length++] = Hex[advert.data[index] & 0x0f];
    }
    payload[length++] = '"';
    payload[length++] = '}';
    payload[length] = '\0';

    mqttSendRaw(mqttTopic(topic).c_str(), payload, false);
}

} // namespace
} // namespace proxy
} // namespace ble
} // namespace espurna

void bleProxySetup() {
    using namespace espurna::ble::proxy;

    interval = espurna::duration::Seconds(getSetting("bleProxyInterval", build::interval()));
    min_rssi = getSetting("bleProxyRssi", build::rssi());
    bleOnAdvert(on_advert);
}

#endif // MODULE_BUILD
