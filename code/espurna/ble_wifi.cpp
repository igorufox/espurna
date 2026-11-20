/*

BLE WI-FI SETUP: Improv Wi-Fi over BLE (improv-wifi.com), the ble_wifi module (owner 2026-11-19, «оба варианта как
модули который может и не быть»: this one and the Matter commissioning over BLE). A phone (the Improv page in Chrome,
Home Assistant's Improv integration) finds the board by its advertisement and writes the name and the password of
the Wi-Fi; the board joins it and answers with its address.

The GATT service of the ble module (ble.h gatt): the Improv service and its five characteristics. Only while the
board is not on a Wi-Fi network (no station connection) it takes new settings: on a network, an RPC gets "not
authorized" (anyone in range could move the board to another network otherwise). The new network goes into the first
slot of the Wi-Fi settings (ssid0 / pass0), saved, the station connects again; no connection in 30 s: "unable to
connect", the board is open for another try.

*/

#include "espurna.h"

#if MODULE_BUILD

#include "ble.h"
#include "espurna_wifi.h"
#include "settings.h"
#include "system.h"

#include <cstring>

namespace espurna {
namespace ble {
namespace wifi {
namespace {

// 00467768-6228-2272-4663-2774782680XX, LSB first
#define IMPROV_UUID(LAST) {16, {LAST, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, \
    0x46, 0x00}}

// the characteristics, in this order (their index in the service)
enum Characteristic : size_t {
    CurrentState,
    ErrorState,
    RpcCommand,
    RpcResult,
    Capabilities,
};

const gatt::Characteristic Characteristics[] {
    {IMPROV_UUID(0x01), gatt::Read | gatt::Notify},
    {IMPROV_UUID(0x02), gatt::Read | gatt::Notify},
    {IMPROV_UUID(0x03), gatt::Write},
    {IMPROV_UUID(0x04), gatt::Read | gatt::Notify},
    {IMPROV_UUID(0x05), gatt::Read},
};

const gatt::Service Service {
    IMPROV_UUID(0x00),
    Characteristics,
    sizeof(Characteristics) / sizeof(Characteristics[0]),
};

enum class State : uint8_t {
    AuthorizationRequired = 0x01,
    Authorized = 0x02,
    Provisioning = 0x03,
    Provisioned = 0x04,
};

enum class Error : uint8_t {
    None = 0x00,
    InvalidRpc = 0x01,
    UnknownRpc = 0x02,
    UnableToConnect = 0x03,
    NotAuthorized = 0x04,
    Unknown = 0xff,
};

constexpr uint8_t CommandWifiSettings { 0x01 };

// the service data of the advertisement, UUID 0x4677
constexpr uint16_t ServiceDataUuid { 0x4677 };

constexpr auto Timeout = espurna::duration::Seconds(30);

State state { State::Authorized };
int service { -1 };
timer::SystemTimer timeout;

uint8_t checksum(const uint8_t* data, size_t length) {
    uint32_t sum = 0;
    for (size_t index = 0; index < length; ++index) {
        sum += data[index];
    }

    return static_cast<uint8_t>(sum & 0xff);
}

// only while the board is off the network: the radio advertises one service at a time (matter_ble has its window)
void advertise() {
    if (state == State::Provisioned) {
        bleAdvertise(service, nullptr, 0, nullptr, 0, 0, 0);
        return;
    }

    uint8_t adv[31];
    size_t length = 0;

    // flags: general discoverable, no BR/EDR
    adv[length++] = 2;
    adv[length++] = 0x01;
    adv[length++] = 0x06;

    // the complete list of the 128-bit services
    adv[length++] = 17;
    adv[length++] = 0x07;
    std::memcpy(&adv[length], Service.uuid.bytes, 16);
    length += 16;

    // the service data: the state, the capabilities, 4 reserved bytes
    adv[length++] = 9;
    adv[length++] = 0x16;
    adv[length++] = ServiceDataUuid & 0xff;
    adv[length++] = ServiceDataUuid >> 8;
    adv[length++] = static_cast<uint8_t>(state);
    adv[length++] = 0;
    adv[length++] = 0;
    adv[length++] = 0;
    adv[length++] = 0;
    adv[length++] = 0;

    // the name in the scan response
    uint8_t response[31];
    const auto hostname = systemHostname();
    const size_t name = std::min<size_t>(hostname.length(), sizeof(response) - 2);
    response[0] = static_cast<uint8_t>(name + 1);
    response[1] = 0x09;
    std::memcpy(&response[2], hostname.c_str(), name);

    bleAdvertise(service, adv, length, response, name + 2, 100, 100);
}

void set_state(State value, bool notify) {
    state = value;
    const uint8_t byte = static_cast<uint8_t>(value);
    bleValue(service, CurrentState, &byte, 1, notify);
    advertise();
}

void set_error(Error value, bool notify) {
    const uint8_t byte = static_cast<uint8_t>(value);
    bleValue(service, ErrorState, &byte, 1, notify);
}

// the RPC result: the command, then the strings (the address of the board)
void result(uint8_t command, const char* url) {
    uint8_t data[gatt::ValueMax];
    const size_t url_length = url ? std::min<size_t>(std::strlen(url), sizeof(data) - 5) : 0;

    size_t length = 0;
    data[length++] = command;
    data[length++] = static_cast<uint8_t>(url ? (url_length + 1) : 0);
    if (url) {
        data[length++] = static_cast<uint8_t>(url_length);
        std::memcpy(&data[length], url, url_length);
        length += url_length;
    }
    data[length] = checksum(data, length);
    ++length;

    bleValue(service, RpcResult, data, length, true);
}

void provisioned() {
    timeout.stop();

    char ip[16];
    wifiStaIp().toString(ip, sizeof(ip));

    char url[24];
    std::snprintf(url, sizeof(url), "http://%s", ip);
    result(CommandWifiSettings, url);
    set_state(State::Provisioned, true);
    DEBUG_MSG_P(PSTR("[BLE] Improv: on the network, %s\n"), url);
}

void wifi_settings(const uint8_t* data, size_t length) {
    // ssid length, ssid, password length, password
    if (length < 2) {
        set_error(Error::InvalidRpc, true);
        return;
    }

    const size_t ssid_length = data[0];
    if ((1 + ssid_length + 1) > length) {
        set_error(Error::InvalidRpc, true);
        return;
    }

    const size_t pass_length = data[1 + ssid_length];
    if ((1 + ssid_length + 1 + pass_length) > length) {
        set_error(Error::InvalidRpc, true);
        return;
    }

    espurna::string ssid(reinterpret_cast<const char*>(&data[1]), ssid_length);
    espurna::string pass(reinterpret_cast<const char*>(&data[1 + ssid_length + 1]), pass_length);

    setSetting({"ssid", 0}, ssid);
    setSetting({"pass", 0}, pass);
    saveSettings();

    set_error(Error::None, true);
    set_state(State::Provisioning, true);
    DEBUG_MSG_P(PSTR("[BLE] Improv: joining \"%s\"\n"), ssid.c_str());

    timeout.once(Timeout, []() {
        if (state == State::Provisioning) {
            set_error(Error::UnableToConnect, true);
            set_state(State::Authorized, true);
            DEBUG_MSG_P(PSTR("[BLE] Improv: no connection\n"));
        }
    });

    wifiDisconnect();
}

// a write of the client, in loop()
void on_write(uint16_t, size_t index, const uint8_t* data, size_t length) {
    if (index != RpcCommand) {
        return;
    }

    // command, data length, data, checksum
    if ((length < 3) || ((2 + data[1] + 1) != length) || (checksum(data, length - 1) != data[length - 1])) {
        set_error(Error::InvalidRpc, true);
        return;
    }

    if (data[0] != CommandWifiSettings) {
        set_error(Error::UnknownRpc, true);
        return;
    }

    // on a network already: no new settings over the air
    if (state != State::Authorized) {
        set_error(Error::NotAuthorized, true);
        return;
    }

    wifi_settings(&data[2], data[1]);
}

void on_connection(uint16_t, bool connected, uint8_t) {
    DEBUG_MSG_P(PSTR("[BLE] Improv: client %s\n"), connected ? PSTR("connected") : PSTR("gone"));
}

void on_wifi(espurna::wifi::Event event) {
    switch (event) {
    case espurna::wifi::Event::StationConnected:
        if (state != State::Provisioned) {
            provisioned();
        }
        break;
    case espurna::wifi::Event::StationDisconnected:
        if (state == State::Provisioned) {
            set_state(State::Authorized, true);
        }
        break;
    default:
        break;
    }
}

} // namespace
} // namespace wifi
} // namespace ble
} // namespace espurna

void bleWifiSetup() {
    using namespace espurna::ble::wifi;

    espurna::ble::gatt::Events events {};
    events.connection = on_connection;
    events.written = on_write;
    service = bleServe(Service, events);
    if (service < 0) {
        DEBUG_MSG_P(PSTR("[BLE] Improv: no room for the service in the GATT server\n"));
        return;
    }

    const uint8_t capabilities = 0;
    bleValue(service, Capabilities, &capabilities, 1, false);
    set_error(Error::None, false);
    set_state(wifiConnected() ? State::Provisioned : State::Authorized, false);

    wifiRegister(on_wifi);
}

#endif // MODULE_BUILD
