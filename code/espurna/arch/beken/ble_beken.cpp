/*

BLE DRIVER of the BK7231N (ble_driver.h): the BLE 5.x stack of the BDK (driver/ble_5_x_rw, libble_bk7231n.a), an
object of the ble module (the Beken line of custom_modules in platformio_beken.ini), nothing of it in the head.

ble_init() + ble_entry() start the stack's task ("ble"). The BDK runs one operation of its app at a time, so the
driver goes step by step from the callbacks: when the stack is ready (BLE_5_STACK_OK), the attribute database of each
service of the GATT server (bk_ble_create_db, one profile per service: prf_id = the service), the scan of one activity,
then the advertising ble.cpp asks for on another (bk_ble_adv_start; changed: the old activity stopped / deleted
first). Every advertisement report and every event of the client (in the stack's task) goes to the queues of loop();
the reads of a client are answered from the values of ble.cpp.

The BDK's scan is the observer one (app_ble_start_scaning: GAPM_SCAN_TYPE_OBSERVER, 1M PHY, no scan requests): bleActive
has no effect here. The exchange memory of the controller is the BLE hardware's own (0x00910000), not the RAM of the
chip.

*/

#include "../../espurna.h"

#if MODULE_BUILD

#include "../../ble_driver.h"
#include "../../espurna_tasks.h"

#include <cstring>

#include "bdk.h"
// the BLE hooks of the SDK's glue (framework-beken-bdk: glue/fixups/ble_off.c)
#include <ble_hooks.h>

extern "C" {
#include <ble_api.h>

// the stack's own (scripts/beken_bdk.py renames them: the head has the names, fixups/ble_off.c)
void bk_stack_ble_init(void);
void bk_stack_ble_entry(void);
uint8_t bk_stack_if_ble_sleep(void);
void bk_stack_ble_switch_rf_to_wifi(void);
void bk_stack_rf_wifi_used_set(void);
void bk_stack_rf_wifi_used_clr(void);
void bk_stack_ps_recover_ble_switch_mac_status(void);

// ble_pub/app/src/app_comm.c
ble_err_t bk_ble_send_ntf_value(uint32_t len, uint8_t* buf, uint16_t prf_id, uint16_t att_idx);
ble_err_t bk_ble_send_ind_value(uint32_t len, uint8_t* buf, uint16_t prf_id, uint16_t att_idx);
} // extern "C"

namespace espurna {
namespace ble {
namespace driver {
namespace {

constexpr uint8_t NoActivity { 0xff };

ScanParams params;
Services services;

espurna::system::Shared<bool> scanning { false };
espurna::system::Shared<uint16_t> client { gatt::NoConnection };
uint8_t scan_activity { NoActivity };

// --- the attribute databases: the service, then per characteristic its declaration, its value and, notified or
// indicated, the client configuration

constexpr size_t AttributesMax { 1 + (gatt::CharacteristicsMax * 3) };

struct Database {
    bk_attm_desc_t attributes[AttributesMax];
    size_t count;
    uint16_t value[gatt::CharacteristicsMax];      // the attribute of each characteristic's value
    uint16_t config[gatt::CharacteristicsMax];     // of its client configuration, 0 - none
};

Database databases[gatt::ServicesMax];
size_t databases_created { 0 };

constexpr uint8_t PrimaryService[16] {0x00, 0x28};
constexpr uint8_t CharacteristicDeclaration[16] {0x03, 0x28};
constexpr uint8_t ClientConfiguration[16] {0x02, 0x29};

void attribute(Database& db, const uint8_t (&uuid)[16], uint16_t perm, uint16_t ext_perm, uint16_t max_size) {
    auto& out = db.attributes[db.count++];
    std::memcpy(out.uuid, uuid, sizeof(out.uuid));
    out.perm = perm;
    out.ext_perm = ext_perm;
    out.max_size = max_size;
}

void table(Database& db, const gatt::Service& service) {
    db.count = 0;
    attribute(db, PrimaryService, BK_PERM_SET(RD, ENABLE), 0, 0);

    for (size_t index = 0; index < service.count; ++index) {
        const auto& in = service.characteristics[index];
        attribute(db, CharacteristicDeclaration, BK_PERM_SET(RD, ENABLE), 0, 0);

        uint16_t perm = 0;
        if (in.props & gatt::Read) {
            perm |= BK_PERM_SET(RD, ENABLE);
        }
        if (in.props & gatt::Write) {
            perm |= BK_PERM_SET(WRITE_REQ, ENABLE) | BK_PERM_SET(WRITE_COMMAND, ENABLE);
        }
        if (in.props & gatt::Notify) {
            perm |= BK_PERM_SET(NTF, ENABLE);
        }
        if (in.props & gatt::Indicate) {
            perm |= BK_PERM_SET(IND, ENABLE);
        }

        uint8_t uuid[16] {};
        std::memcpy(uuid, in.uuid.bytes, (in.uuid.size == 2) ? 2 : 16);
        const uint16_t length = (in.uuid.size == 2)
            ? BK_PERM_SET(UUID_LEN, UUID_16)
            : BK_PERM_SET(UUID_LEN, UUID_128);

        db.value[index] = static_cast<uint16_t>(db.count);
        attribute(db, uuid, perm, BK_PERM_SET(RI, ENABLE) | length, gatt::DataMax);

        db.config[index] = 0;
        if (in.props & (gatt::Notify | gatt::Indicate)) {
            db.config[index] = static_cast<uint16_t>(db.count);
            attribute(db, ClientConfiguration, BK_PERM_SET(RD, ENABLE) | BK_PERM_SET(WRITE_REQ, ENABLE), 0, 0);
        }
    }
}

// the characteristic of an attribute of a service: its index, -1 - none; config: it is the client configuration
int characteristic(size_t service, uint16_t att_idx, bool& config) {
    if (service >= services.count) {
        return -1;
    }

    const auto& db = databases[service];
    for (size_t index = 0; index < services.service[service]->count; ++index) {
        if (db.value[index] == att_idx) {
            config = false;
            return static_cast<int>(index);
        }
        if (db.config[index] && (db.config[index] == att_idx)) {
            config = true;
            return static_cast<int>(index);
        }
    }

    return -1;
}

bool create_database(size_t service) {
    const auto& in = *services.service[service];
    auto& db = databases[service];
    table(db, in);

    bk_ble_db_cfg cfg{};
    cfg.prf_task_id = static_cast<uint16_t>(service);
    std::memset(cfg.uuid, 0, sizeof(cfg.uuid));
    std::memcpy(cfg.uuid, in.uuid.bytes, (in.uuid.size == 2) ? 2 : 16);
    cfg.att_db_nb = static_cast<uint8_t>(db.count);
    cfg.start_hdl = 0;
    cfg.att_db = db.attributes;
    cfg.svc_perm = (in.uuid.size == 2)
        ? BK_PERM_SET(SVC_UUID_LEN, UUID_16)
        : BK_PERM_SET(SVC_UUID_LEN, UUID_128);

    return bk_ble_create_db(&cfg) == ERR_SUCCESS;
}

// --- advertising: what ble.cpp wants, what is on the air

struct Advertising {
    bool active;
    adv_param param;
};

Advertising wanted {};
uint8_t adv_activity { NoActivity };
bool adv_running { false };
espurna::system::Shared<bool> adv_busy { false };
espurna::system::Shared<bool> adv_dirty { false };
bool ready { false };

// 0.625 ms units
uint16_t slots(uint16_t ms) {
    return static_cast<uint16_t>((static_cast<uint32_t>(ms) * 8) / 5);
}

void apply_advertising();

void adv_done(ble_cmd_t cmd, ble_cmd_param_t* param) {
    const bool ok = param && (param->status == ERR_SUCCESS);
    switch (cmd) {
    // bk_ble_adv_start() runs create + data + response + start as one: BLE_INIT_ADV
    case BLE_INIT_ADV:
    case BLE_START_ADV:
        adv_running = ok;
        break;
    case BLE_STOP_ADV:
    case BLE_DELETE_ADV:
        adv_running = false;
        if (cmd == BLE_DELETE_ADV) {
            adv_activity = NoActivity;
        }
        break;
    default:
        break;
    }

    adv_busy.store(false);
    if (adv_dirty.exchange(false)) {
        apply_advertising();
    }
}

// one step towards what is wanted; the callback takes the next one
void apply_advertising() {
    if (!ready || adv_busy.exchange(true)) {
        adv_dirty.store(true);
        return;
    }

    const bool want = wanted.active && (client.load() == gatt::NoConnection);

    // a change: the old activity goes first
    if (adv_activity != NoActivity) {
        if (adv_running) {
            bk_ble_stop_advertising(adv_activity, adv_done);
            adv_dirty.store(true);
            return;
        }
        bk_ble_delete_advertising(adv_activity, adv_done);
        adv_dirty.store(want);
        return;
    }

    if (!want) {
        adv_busy.store(false);
        return;
    }

    adv_activity = app_ble_get_idle_actv_idx_handle();
    auto param = wanted.param;
    if (bk_ble_adv_start(adv_activity, &param, adv_done) != ERR_SUCCESS) {
        adv_activity = NoActivity;
        adv_busy.store(false);
    }
}

// --- the scan

void scan_started(ble_cmd_t, ble_cmd_param_t* param) {
    if (param && (param->status == ERR_SUCCESS)) {
        scanning.store(true);
    }

    // the scan first, then the advertising
    ready = true;
    apply_advertising();
}

void start_scan() {
    scan_activity = app_ble_get_idle_actv_idx_handle();

    scan_param scan{};
    scan.filter_en = 0;
    scan.channel_map = 7; // 37, 38, 39
    scan.interval = slots(params.interval_ms);
    scan.window = slots(params.window_ms);

    if (bk_ble_scan_start(scan_activity, &scan, scan_started) != ERR_SUCCESS) {
        ready = true;
        apply_advertising();
    }
}

void report(const recv_adv_t& report) {
    Advert advert;
    // the BD address of the stack is LSB first
    for (size_t index = 0; index < sizeof(advert.mac); ++index) {
        advert.mac[index] = report.adv_addr[sizeof(advert.mac) - 1 - index];
    }

    advert.addr_type = report.adv_addr_type;
    advert.rssi = static_cast<int8_t>(report.rssi);
    advert.length = static_cast<uint8_t>(std::min<size_t>(report.data_len, sizeof(advert.data)));
    std::memcpy(advert.data, report.data, advert.length);

    received(advert);
}

// the characteristic of the last indication (one at a time): service << 8 | index
espurna::system::Shared<uint16_t> indicating { 0xffff };

// the next database, or the scan when they are all there
void next_database() {
    while (databases_created < services.count) {
        if (create_database(databases_created++)) {
            return;
        }
    }

    start_scan();
}

// the stack's task
void notice(ble_notice_t notice, void* param) {
    switch (notice) {
    case BLE_5_STACK_OK:
        databases_created = 0;
        next_database();
        break;

    case BLE_5_CREATE_DB:
        next_database();
        break;

    case BLE_5_REPORT_ADV:
        if (param) {
            report(*static_cast<const recv_adv_t*>(param));
        }
        break;

    case BLE_5_READ_EVENT:
        if (param) {
            auto& request = *static_cast<read_req_t*>(param);
            bool config = false;
            const int index = characteristic(request.prf_id, request.att_idx, config);
            request.length = ((index >= 0) && !config)
                ? static_cast<uint16_t>(read(request.prf_id, index, request.value, request.size))
                : 0;
        }
        break;

    case BLE_5_WRITE_EVENT:
        if (param) {
            const auto& request = *static_cast<const write_req_t*>(param);
            bool config = false;
            const int index = characteristic(request.prf_id, request.att_idx, config);
            if (index < 0) {
                break;
            }
            if (config) {
                const uint8_t bits = request.len ? request.value[0] : 0;
                subscribed(request.prf_id, index, request.conn_idx, (bits & 1) != 0, (bits & 2) != 0);
            } else {
                written(request.prf_id, index, request.conn_idx, request.value, request.len);
            }
        }
        break;

    case BLE_5_ATT_INFO_REQ:
        if (param) {
            auto& request = *static_cast<att_info_req_t*>(param);
            request.length = gatt::DataMax;
            request.status = ERR_SUCCESS;
        }
        break;

    case BLE_5_MTU_CHANGE:
        if (param) {
            const auto& change = *static_cast<const mtu_change_t*>(param);
            mtu(change.conn_idx, change.mtu_size);
        }
        break;

    case BLE_5_TX_DONE: {
        const uint16_t key = indicating.exchange(0xffff);
        if (key != 0xffff) {
            indicated(key >> 8, key & 0xff, client.load(), true);
        }
        break;
    }

    case BLE_5_CONNECT_EVENT:
        if (param) {
            const auto conn = static_cast<const conn_ind_t*>(param)->conn_idx;
            client.store(conn);
            // the BDK stops advertising on a connection
            adv_running = false;
            connection(conn, true, 0);
            bk_ble_gatt_mtu_change(conn, nullptr);
        }
        break;

    case BLE_5_DISCONNECT_EVENT:
        if (param) {
            const auto& ind = *static_cast<const discon_ind_t*>(param);
            client.store(gatt::NoConnection);
            connection(ind.conn_idx, false, ind.reason);
        }
        apply_advertising();
        break;

    default:
        break;
    }
}

} // namespace

// what the head's Wi-Fi asks the BLE from now on
const ble_hooks Hooks {
    bk_stack_if_ble_sleep,
    bk_stack_ble_switch_rf_to_wifi,
    bk_stack_rf_wifi_used_set,
    bk_stack_rf_wifi_used_clr,
    bk_stack_ps_recover_ble_switch_mac_status,
};

bool begin(const ScanParams& scan, const Services& config) {
    params = scan;
    services = config;

    ble_hooks_set(&Hooks);
    ble_set_notice_cb(notice);
    // the FIQs of the radio and the device "ble" (ble_main.c), then the stack's task
    bk_stack_ble_init();
    bk_stack_ble_entry();
    return true;
}

bool running() {
    return scanning.load();
}

void advertise(const uint8_t* data, size_t length, const uint8_t* response, size_t response_length,
    uint16_t min_ms, uint16_t max_ms)
{
    Advertising next {};
    next.active = data != nullptr;
    if (next.active) {
        auto& param = next.param;
        param.advDataLen = static_cast<uint8_t>(std::min<size_t>(length, sizeof(param.advData)));
        std::memcpy(param.advData, data, param.advDataLen);
        param.respDataLen = static_cast<uint8_t>(response ? std::min<size_t>(response_length, sizeof(param.respData)) : 0);
        if (param.respDataLen) {
            std::memcpy(param.respData, response, param.respDataLen);
        }
        param.channel_map = 7;
        param.interval_min = slots(min_ms ? min_ms : 100);
        param.interval_max = slots(max_ms ? max_ms : (min_ms ? min_ms : 100));
        param.duration = 0;
    }

    wanted = next;
    apply_advertising();
}

void notify(size_t service, size_t index) {
    if ((client.load() == gatt::NoConnection) || (service >= services.count)
        || (index >= services.service[service]->count))
    {
        return;
    }

    uint8_t buffer[gatt::ValueMax];
    const size_t length = read(service, index, buffer, sizeof(buffer));
    bk_ble_send_ntf_value(length, buffer, static_cast<uint16_t>(service), databases[service].value[index]);
}

bool indicate(size_t service, size_t index, uint16_t conn, const uint8_t* data, size_t length) {
    if ((conn == gatt::NoConnection) || (service >= services.count) || (index >= services.service[service]->count)) {
        return false;
    }

    uint8_t buffer[gatt::DataMax];
    const size_t size = std::min(length, sizeof(buffer));
    std::memcpy(buffer, data, size);

    indicating.store(static_cast<uint16_t>((service << 8) | index));
    if (bk_ble_send_ind_value(size, buffer, static_cast<uint16_t>(service), databases[service].value[index])
        != ERR_SUCCESS)
    {
        indicating.store(0xffff);
        return false;
    }

    return true;
}

void disconnect(uint16_t conn) {
    bk_ble_disconnect(static_cast<uint8_t>(conn), nullptr);
}

} // namespace driver
} // namespace ble
} // namespace espurna

#endif // MODULE_BUILD
