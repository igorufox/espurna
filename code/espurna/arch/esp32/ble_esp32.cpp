/*

BLE DRIVER of the ESP32 (ble_driver.h): the controller in ROM + libbtdm_app and the NimBLE host of ESP-IDF, an object
of the ble module (the ESP32 line of custom_modules in platformio_esp32.ini), nothing of it in the head.

nimble_port_init() brings the controller up in BLE mode (esp_bt_controller_init / enable) and the host; the services of
the GATT server are registered before the host runs (ble_gatts_add_svcs); the host runs in a task of its own
(nimble_port_freertos_init), its sync callback starts the discovery (the observer) and the advertising ble.cpp asked
for (connectable, one client). Every report (BLE_GAP_EVENT_DISC) and every event of the client (in the host's task) goes
to the queues of loop(). The MTU asked for: gatt::Mtu (CHIPoBLE wants 247).

The controller writes into fixed RAM below the head's data (the BT exchange memory at 0x3FFB0000, ESP-IDF
SOC_MEM_BT_EM_START): the head keeps that place for it (arch/esp32/ble_ram_esp32.cpp).

*/

#include "../../espurna.h"

#if MODULE_BUILD

#include "../../ble_driver.h"
#include "../../espurna_tasks.h"

#include <cstring>

#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

// the head, arch/esp32/ble_ram_esp32.cpp: the RAM of the controller in ROM is still the controller's (the ble module was
// on at boot)
bool bleRamKept();

namespace espurna {
namespace ble {
namespace driver {
namespace {

ScanParams params;
Services services;

espurna::system::Shared<bool> scanning { false };
espurna::system::Shared<bool> synced { false };
espurna::system::Shared<uint16_t> client { gatt::NoConnection };
uint8_t own_addr_type { 0 };

// the advertisement on the air (a copy: advertise() runs in loop(), the host's task starts it again)
struct Advertising {
    bool active;
    uint8_t data[AdvertDataMax];
    size_t length;
    uint8_t response[AdvertDataMax];
    size_t response_length;
    uint16_t min_ms;
    uint16_t max_ms;
};

Advertising advertising {};

// the GATT tables (NimBLE keeps pointers into them)
union Uuid {
    ble_uuid_t u;
    ble_uuid16_t u16;
    ble_uuid128_t u128;
};

Uuid service_uuids[gatt::ServicesMax];
Uuid characteristic_uuids[gatt::ServicesMax][gatt::CharacteristicsMax];
ble_gatt_chr_def characteristics[gatt::ServicesMax][gatt::CharacteristicsMax + 1];
ble_gatt_svc_def definitions[gatt::ServicesMax + 1];
uint16_t handles[gatt::ServicesMax][gatt::CharacteristicsMax];

// the characteristic of the last indication (one at a time)
espurna::system::Shared<uint16_t> indicating { 0xffff };

// 0.625 ms units
uint16_t slots(uint16_t ms) {
    return static_cast<uint16_t>((static_cast<uint32_t>(ms) * 8) / 5);
}

void uuid(Uuid& out, const gatt::Uuid& in) {
    std::memset(&out, 0, sizeof(out));
    if (in.size == 2) {
        out.u16.u.type = BLE_UUID_TYPE_16;
        out.u16.value = static_cast<uint16_t>(in.bytes[0] | (in.bytes[1] << 8));
    } else {
        out.u128.u.type = BLE_UUID_TYPE_128;
        std::memcpy(out.u128.value, in.bytes, sizeof(out.u128.value));
    }
}

// the access callback argument: service << 8 | index
int access(uint16_t conn, uint16_t, ble_gatt_access_ctxt* ctxt, void* arg) {
    const auto key = reinterpret_cast<uintptr_t>(arg);
    const size_t service = key >> 8;
    const size_t index = key & 0xff;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        uint8_t buffer[gatt::ValueMax];
        const size_t length = read(service, index, buffer, sizeof(buffer));
        return (os_mbuf_append(ctxt->om, buffer, length) == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buffer[gatt::DataMax];
        uint16_t length { 0 };
        if (ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &length) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        written(service, index, conn, buffer, length);
        return 0;
    }

    default:
        break;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

void tables() {
    std::memset(definitions, 0, sizeof(definitions));
    for (size_t service = 0; service < services.count; ++service) {
        const auto& in = *services.service[service];
        uuid(service_uuids[service], in.uuid);

        auto* chars = characteristics[service];
        std::memset(chars, 0, sizeof(characteristics[service]));
        for (size_t index = 0; index < in.count; ++index) {
            const auto& characteristic = in.characteristics[index];
            uuid(characteristic_uuids[service][index], characteristic.uuid);

            auto& out = chars[index];
            out.uuid = &characteristic_uuids[service][index].u;
            out.access_cb = access;
            out.arg = reinterpret_cast<void*>(static_cast<uintptr_t>((service << 8) | index));
            out.val_handle = &handles[service][index];
            out.flags = ((characteristic.props & gatt::Read) ? BLE_GATT_CHR_F_READ : 0)
                | ((characteristic.props & gatt::Write) ? (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP) : 0)
                | ((characteristic.props & gatt::Notify) ? BLE_GATT_CHR_F_NOTIFY : 0)
                | ((characteristic.props & gatt::Indicate) ? BLE_GATT_CHR_F_INDICATE : 0);
        }

        definitions[service].type = BLE_GATT_SVC_TYPE_PRIMARY;
        definitions[service].uuid = &service_uuids[service].u;
        definitions[service].characteristics = chars;
    }
}

// the characteristic of a value handle: service << 8 | index, 0xffff - none
uint16_t characteristic(uint16_t handle) {
    for (size_t service = 0; service < services.count; ++service) {
        for (size_t index = 0; index < services.service[service]->count; ++index) {
            if (handles[service][index] == handle) {
                return static_cast<uint16_t>((service << 8) | index);
            }
        }
    }

    return 0xffff;
}

int gap_event(ble_gap_event* event, void*);

// in the host's task (connection events) or in loop() (advertise())
void start_advertising() {
    if (!synced.load() || !advertising.active || (client.load() != gatt::NoConnection)) {
        return;
    }

    ble_gap_adv_stop();
    ble_gap_adv_set_data(advertising.data, advertising.length);
    if (advertising.response_length) {
        ble_gap_adv_rsp_set_data(advertising.response, advertising.response_length);
    }

    ble_gap_adv_params adv{};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv.itvl_min = slots(advertising.min_ms);
    adv.itvl_max = slots(advertising.max_ms);
    ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &adv, gap_event, nullptr);
}

void start_scan() {
    ble_gap_disc_params disc{};
    disc.itvl = slots(params.interval_ms);
    disc.window = slots(params.window_ms);
    disc.filter_policy = 0;
    disc.limited = 0;
    disc.passive = params.active ? 0 : 1;
    disc.filter_duplicates = 0;

    if (ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc, gap_event, nullptr) == 0) {
        scanning.store(true);
    }
}

int gap_event(ble_gap_event* event, void*) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const auto& disc = event->disc;

        Advert advert;
        // NimBLE's address is LSB first
        for (size_t index = 0; index < sizeof(advert.mac); ++index) {
            advert.mac[index] = disc.addr.val[sizeof(advert.mac) - 1 - index];
        }

        advert.addr_type = disc.addr.type;
        advert.rssi = disc.rssi;
        advert.length = static_cast<uint8_t>(std::min<size_t>(disc.length_data, sizeof(advert.data)));
        std::memcpy(advert.data, disc.data, advert.length);

        received(advert);
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        scanning.store(false);
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            client.store(event->connect.conn_handle);
            connection(event->connect.conn_handle, true, 0);
        } else {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        client.store(gatt::NoConnection);
        connection(event->disconnect.conn.conn_handle, false, static_cast<uint8_t>(event->disconnect.reason & 0xff));
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        const uint16_t key = characteristic(event->subscribe.attr_handle);
        if (key != 0xffff) {
            subscribed(key >> 8, key & 0xff, event->subscribe.conn_handle,
                event->subscribe.cur_notify != 0, event->subscribe.cur_indicate != 0);
        }
        break;
    }

    case BLE_GAP_EVENT_MTU:
        mtu(event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.indication && (event->notify_tx.status != 0)) {
            // BLE_HS_EDONE: confirmed; anything else: failed or timed out
            const uint16_t key = indicating.exchange(0xffff);
            if (key != 0xffff) {
                indicated(key >> 8, key & 0xff, event->notify_tx.conn_handle,
                    event->notify_tx.status == BLE_HS_EDONE);
            }
        }
        break;

    default:
        break;
    }

    return 0;
}

// the host's task
void on_sync() {
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return;
    }

    synced.store(true);
    start_scan();
    start_advertising();
}

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

} // namespace

bool begin(const ScanParams& scan, const Services& config) {
    params = scan;
    services = config;

    if (!bleRamKept()) {
        DEBUG_MSG_P(PSTR("[BLE] Switched on after boot, starts with the next one\n"));
        return false;
    }

    if (nimble_port_init() != ESP_OK) {
        return false;
    }

    if (services.count) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
        ble_svc_gap_device_name_set(systemHostname().c_str());
        ble_att_set_preferred_mtu(gatt::Mtu);

        tables();
        if ((ble_gatts_count_cfg(definitions) != 0) || (ble_gatts_add_svcs(definitions) != 0)) {
            DEBUG_MSG_P(PSTR("[BLE] GATT server: the services did not register\n"));
            services.count = 0;
        }
    }

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return true;
}

bool running() {
    return scanning.load();
}

void advertise(const uint8_t* data, size_t length, const uint8_t* response, size_t response_length,
    uint16_t min_ms, uint16_t max_ms)
{
    if (!data) {
        advertising.active = false;
        if (synced.load()) {
            ble_gap_adv_stop();
        }
        return;
    }

    advertising.active = true;
    advertising.length = std::min(length, AdvertDataMax);
    std::memcpy(advertising.data, data, advertising.length);
    advertising.response_length = std::min(response_length, AdvertDataMax);
    if (advertising.response_length) {
        std::memcpy(advertising.response, response, advertising.response_length);
    }
    advertising.min_ms = min_ms ? min_ms : 100;
    advertising.max_ms = max_ms ? max_ms : advertising.min_ms;

    start_advertising();
}

void notify(size_t service, size_t index) {
    const uint16_t conn = client.load();
    if ((conn == gatt::NoConnection) || (service >= services.count) || (index >= services.service[service]->count)) {
        return;
    }

    uint8_t buffer[gatt::ValueMax];
    const size_t length = read(service, index, buffer, sizeof(buffer));
    auto* om = ble_hs_mbuf_from_flat(buffer, length);
    if (om) {
        ble_gatts_notify_custom(conn, handles[service][index], om);
    }
}

bool indicate(size_t service, size_t index, uint16_t conn, const uint8_t* data, size_t length) {
    if ((service >= services.count) || (index >= services.service[service]->count)) {
        return false;
    }

    auto* om = ble_hs_mbuf_from_flat(data, length);
    if (!om) {
        return false;
    }

    indicating.store(static_cast<uint16_t>((service << 8) | index));
    if (ble_gatts_indicate_custom(conn, handles[service][index], om) != 0) {
        indicating.store(0xffff);
        return false;
    }

    return true;
}

void disconnect(uint16_t conn) {
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

} // namespace driver
} // namespace ble
} // namespace espurna

#endif // MODULE_BUILD
