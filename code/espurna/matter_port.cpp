/*

MATTER: THE PLATFORM LAYER UNDER THE LIBRARY, ONE CODE FOR EVERY CHIP

The Matter library (matter-espurna) has one platform layer for every chip, external_platform/Port: what it needs from
the firmware is the matter_port_* API of its matter_port.h, implemented here once:
- the key-value store of the stack (chip-config, chip-counters, the KVS of fabrics, ACL ...) and of the board's
  identity (namespace "espurna-id", matter_identity.cpp): FlashDB on the 'kvs' partition of the chip's flash layout
  (FAL: arch/<chip>/flashdb, scripts/flashdb.py). "chip-factory" reads the factory data of the board first, read only
  (backend: identity::factory_read(), the Beken's stock Quectel / Tuya data)
- Wi-Fi is ESPurna's: the station's state as events, its link, its MAC and netif; a network a controller gives goes to
  ESPurna's settings (matterWifiFromController()); the scan is ESPurna's
- the update a controller sends: ESPurna's own OTA path (ota_driver.h)
- the log, the reboot, the size of the heap (backend::heap_total())
What only a chip has stays in arch/<chip>/matter_<chip>.cpp (backend::*: espurna_matter.h).

*/

#include "espurna.h"

#if MATTER_SUPPORT

#include "espurna_matter.h"
#include "espurna_tasks.h"
#include "espurna_wifi.h"
#include "matter_identity.h"
#include "ota_driver.h"
#include "terminal.h"

#include <wifi_sdk.h>

#include <external_platform/Port/matter_port.h>

#include <flashdb.h>

#include <lwip/netif.h>
#include <lwip/tcpip.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace espurna {
namespace matter {
namespace port {
namespace {

// ---------------------------------------------------------------------------------------------------------------------

namespace kv {

// the 'kvs' partition of the chip's flash layout (FAL of arch/<chip>/flashdb), 32 KiB
struct fdb_kvdb db;
espurna::system::Mutex mutex;
bool ready { false };

void lock(fdb_db_t) {
    mutex.lock();
}

void unlock(fdb_db_t) {
    mutex.unlock();
}

bool init() {
    if (ready) {
        return true;
    }

    fdb_kvdb_control(&db, FDB_KVDB_CTRL_SET_LOCK, reinterpret_cast<void*>(lock));
    fdb_kvdb_control(&db, FDB_KVDB_CTRL_SET_UNLOCK, reinterpret_cast<void*>(unlock));

    ready = fdb_kvdb_init(&db, "matter", "kvs", nullptr, nullptr) == FDB_NO_ERR;
    return ready;
}

espurna::string key(const char* ns, const char* name) {
    espurna::string out;
    out.reserve(std::strlen(ns) + std::strlen(name) + 1);
    out += ns;
    out += '/';
    out += name;

    return out;
}

int read(const char* ns, const char* name, void* buf, size_t len, size_t* out_len) {
    if (!init()) {
        return MATTER_PORT_FAIL;
    }

    struct fdb_kv obj;
    const auto full = key(ns, name);
    if (!fdb_kv_get_obj(&db, full.c_str(), &obj)) {
        return MATTER_PORT_FAIL;
    }

    // a read without a buffer: the size only
    const auto copy = buf ? std::min(len, static_cast<size_t>(obj.value_len)) : static_cast<size_t>(obj.value_len);
    if (buf && copy) {
        struct fdb_blob blob;
        fdb_blob_read(reinterpret_cast<fdb_db_t>(&db), fdb_kv_to_blob(&obj, fdb_blob_make(&blob, buf, copy)));
    }

    if (out_len) {
        *out_len = copy;
    }

    return MATTER_PORT_OK;
}

int write(const char* ns, const char* name, const void* buf, size_t len) {
    if (!init()) {
        return MATTER_PORT_FAIL;
    }

    struct fdb_blob blob;
    const auto full = key(ns, name);
    const auto start = espurna::time::ms();
    const auto out = (fdb_kv_set_blob(&db, full.c_str(), fdb_blob_make(&blob, buf, len)) == FDB_NO_ERR)
        ? MATTER_PORT_OK : MATTER_PORT_FAIL;

    // W50: the Matter task held its lock 8.5 s in the flash driver at the start: how long each write takes
    const auto took = espurna::time::ms() - start;
    static uint32_t writes { 0 };
    static uint32_t total { 0 };
    ++writes;
    total += took;
    if (took >= 100) {
        char line[96];
        snprintf(line, sizeof(line), "KVS write %s (%u bytes) took %u ms, %u writes %u ms so far",
            full.c_str(), static_cast<unsigned>(len), static_cast<unsigned>(took),
            static_cast<unsigned>(writes), static_cast<unsigned>(total));
        matter_log(line);
    }

    return out;
}

int remove(const char* ns, const char* name) {
    if (!init()) {
        return MATTER_PORT_FAIL;
    }

    const auto full = key(ns, name);
    return (fdb_kv_del(&db, full.c_str()) == FDB_NO_ERR) ? MATTER_PORT_OK : MATTER_PORT_FAIL;
}

bool exists(const char* ns, const char* name) {
    if (!init()) {
        return false;
    }

    struct fdb_kv obj;
    const auto full = key(ns, name);
    return fdb_kv_get_obj(&db, full.c_str(), &obj) != nullptr;
}

// every key whose name passes the filter
template <typename T>
int remove_if(T&& filter) {
    if (!init()) {
        return MATTER_PORT_FAIL;
    }

    std::vector<espurna::string> names;

    struct fdb_kv_iterator iterator;
    fdb_kv_iterator_init(&iterator);
    while (fdb_kv_iterate(&db, &iterator)) {
        if (filter(iterator.curr_kv.name)) {
            names.emplace_back(iterator.curr_kv.name);
        }
    }

    for (const auto& name : names) {
        fdb_kv_del(&db, name.c_str());
    }

    return MATTER_PORT_OK;
}

int clear(const char* ns) {
    espurna::string prefix(ns);
    prefix += '/';
    return remove_if([&](const char* name) {
        return std::strncmp(name, prefix.c_str(), prefix.length()) == 0;
    });
}

// Matter factory reset: fabrics, ACL, counters... everything but the own identity of the board (namespace
// "espurna-id" of matter_identity.cpp), so the pairing code stays the same
constexpr char IdentityNamespace[] = "espurna-id";

int clear_all() {
    constexpr size_t Length { sizeof(IdentityNamespace) - 1 };
    return remove_if([](const char* name) {
        return (std::strncmp(name, IdentityNamespace, Length) != 0) || (name[Length] != '/');
    });
}

} // namespace kv

// ---------------------------------------------------------------------------------------------------------------------

// The library registers its network callbacks from the Matter task (add() / remove()), the loop reports the network
// to them (network()): the list and the state are only touched under the mutex, the callbacks themselves run outside
// of it, from a copy (they may register again)
namespace events {

struct Callback {
    matter_port_event_module_t module;
    int id;
    matter_port_event_cb_t callback;
    void* arg;
};

espurna::system::Mutex mutex;
std::vector<Callback> callbacks;
bool connected { false };

bool wanted(const Callback& callback, matter_port_event_module_t module, int id) {
    return (callback.module == module)
        && ((callback.id == MATTER_PORT_EVENT_ID_ALL) || (callback.id == id));
}

// the loop
void notify(matter_port_event_module_t module, int id, void* data) {
    std::vector<Callback> copy;
    {
        espurna::system::MutexLock lock(mutex);
        copy = callbacks;
    }

    for (const auto& callback : copy) {
        if (wanted(callback, module, id)) {
            callback.callback(callback.arg, module, id, data);
        }
    }
}

bool up() {
    espurna::system::MutexLock lock(mutex);
    return connected;
}

// the Matter task
void add(matter_port_event_module_t module, int id, matter_port_event_cb_t callback, void* arg) {
    bool up;
    {
        espurna::system::MutexLock lock(mutex);
        for (auto& entry : callbacks) {
            if ((entry.module == module) && (entry.callback == callback)) {
                entry.id = id;
                entry.arg = arg;
                return;
            }
        }

        callbacks.push_back(Callback{module, id, callback, arg});
        up = connected;
    }

    // Matter starts after ESPurna is already on the network
    if (up) {
        const auto event = (module == MATTER_PORT_EVENT_NETIF)
            ? static_cast<int>(MATTER_PORT_EVENT_NETIF_GOT_IP4)
            : static_cast<int>(MATTER_PORT_EVENT_WIFI_STA_CONNECTED);
        if ((id == MATTER_PORT_EVENT_ID_ALL) || (id == event)) {
            callback(arg, module, event, nullptr);
        }
    }
}

void remove(matter_port_event_module_t module, matter_port_event_cb_t callback) {
    espurna::system::MutexLock lock(mutex);
    for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
        if (((*it).module == module) && ((*it).callback == callback)) {
            callbacks.erase(it);
            return;
        }
    }
}

// the loop
void network(bool up) {
    if (up) {
        {
            espurna::system::MutexLock lock(mutex);
            connected = true;
        }
        notify(MATTER_PORT_EVENT_WIFI, MATTER_PORT_EVENT_WIFI_STA_CONNECTED, nullptr);
        notify(MATTER_PORT_EVENT_NETIF, MATTER_PORT_EVENT_NETIF_GOT_IP4, nullptr);
        return;
    }

    bool was;
    {
        espurna::system::MutexLock lock(mutex);
        was = connected;
        connected = false;
    }

    if (was) {
        matter_port_sta_disconnected_t data {};
        notify(MATTER_PORT_EVENT_WIFI, MATTER_PORT_EVENT_WIFI_STA_DISCONNECTED, &data);
    }
}

} // namespace events

// ---------------------------------------------------------------------------------------------------------------------

// ESPurna's scan (wifiScanStart()) from the loop; its results for the Matter task, under the mutex
namespace scan {

espurna::system::Mutex mutex;
std::vector<matter_port_scan_ap_t> found;
espurna::string ssid;

void on_found(const espurna::string& name, int8_t rssi, bool open) {
    espurna::system::MutexLock lock(mutex);
    if (ssid.length() && (name != ssid)) {
        return;
    }

    matter_port_scan_ap_t ap {};
    std::strncpy(ap.ssid, name.c_str(), sizeof(ap.ssid) - 1);
    ap.rssi = rssi;
    ap.security = open ? MATTER_PORT_SECURITY_NONE : MATTER_PORT_SECURITY_WPA2;
    found.push_back(ap);
}

// the end of the scan: an empty text when it went well
void on_done(const espurna::string&) {
    events::notify(MATTER_PORT_EVENT_WIFI, MATTER_PORT_EVENT_WIFI_SCAN_DONE, nullptr);
}

} // namespace scan

// ---------------------------------------------------------------------------------------------------------------------

#if TERMINAL_SUPPORT
// What the 'kvs' partition holds: a write of the Matter task that took 8.5 s (chip-counters/reboot-count at the start,
// the loop watchdog fired, 2026-11-14) points at a full FlashDB that collects garbage on every write
PROGMEM_STRING(Kvs, "MATTER.KVS");

void kvs_report(::terminal::CommandContext&& ctx) {
    if (!kv::init()) {
        terminalError(ctx, "No FlashDB on the 'kvs' partition");
        return;
    }

    size_t count { 0 };
    size_t used { 0 };
    struct fdb_kv_iterator iterator;
    fdb_kv_iterator_init(&iterator);
    while (fdb_kv_iterate(&kv::db, &iterator)) {
        ++count;
        used += iterator.curr_kv.len;
        ctx.output.printf_P(PSTR("%s: value %u, record %u\n"), iterator.curr_kv.name,
            static_cast<unsigned>(iterator.curr_kv.value_len), static_cast<unsigned>(iterator.curr_kv.len));
    }

    ctx.output.printf_P(PSTR("%u keys, %u bytes of live records; partition %u bytes, sector %u\n"),
        static_cast<unsigned>(count), static_cast<unsigned>(used),
        static_cast<unsigned>(kv::db.parent.max_size), static_cast<unsigned>(kv::db.parent.sec_size));
    terminalOK(ctx);
}

constexpr ::terminal::Command Commands[] PROGMEM {
    {Kvs, kvs_report},
};
#endif

} // namespace

void setup() {
    // Matter runs over IPv6 (link-local at least): through the common Wi-Fi, which also adds the address at once when
    // the station is connected already (the Matter module starts after wifiSetup()); the Beken's port adds it itself
    wifiIpv6(true);
#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
}

bool prepare() {
    return kv::init();
}

void network(bool up) {
    events::network(up);
}

// everything but the own identity of the board (namespace "espurna-id", matter_identity.cpp), so the pairing code stays
bool reset() {
    return kv::clear_all() == MATTER_PORT_OK;
}

namespace {

bool factory_value(const char* name, std::vector<uint8_t>& out) {
    size_t length { 0 };
    if (!identity::factory_read(name, nullptr, 0, length)) {
        return false;
    }

    out.resize(length);
    return identity::factory_read(name, out.data(), out.size(), length);
}

// little endian, as the factory data keeps the numbers
uint32_t factory_number(const char* name) {
    std::vector<uint8_t> value;
    if (!factory_value(name, value) || (value.size() > 4)) {
        return 0;
    }

    uint32_t out { 0 };
    for (size_t index = value.size(); index > 0; --index) {
        out = (out << 8) | value[index - 1];
    }

    return out;
}

espurna::string factory_text(const char* name) {
    std::vector<uint8_t> value;
    espurna::string out;
    if (factory_value(name, value)) {
        out.concat(reinterpret_cast<const char*>(value.data()), value.size());
    }

    return out;
}

size_t factory_size(const char* name) {
    size_t length { 0 };
    identity::factory_read(name, nullptr, 0, length);
    return length;
}

} // namespace

void describe(Identity& out) {
    const auto source = identity::factory_source();
    // no factory data, or `matterIdentity` chose the own or the custom identity (matter_identity.cpp)
    if (!source.length() || (identity::choice() != identity::Choice::Factory)) {
        out.vendor = "ESPurna";
        out.vendor_id = 0xfff1;
        out.product = systemDevice().toString();
        out.product_id = 0x8000;
        identity::ids(out.vendor_id, out.product_id);
        out.source = identity::status();
        if (!source.length()) {
            out.source += ", no factory data";
        }
        return;
    }

    out.vendor = factory_text("vendor-name");
    out.vendor_id = factory_number("vendor-id");
    out.product = factory_text("product-name");
    out.product_id = factory_number("product-id");
    out.source = "factory identity, ";
    out.source += source;

    out.details = "certificates: DAC ";
    out.details += espurna::string(factory_size("dac-cert"));
    out.details += ", PAI ";
    out.details += espurna::string(factory_size("pai-cert"));
    out.details += ", declaration ";
    out.details += espurna::string(factory_size("cert-dclrn"));
    out.details += " bytes";
}

} // namespace port

// The store of the board's identity (matter_identity.cpp): the key-value store, namespace "espurna-id" next to the
// stack's own
namespace identity {

bool store_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    size_t read { 0 };
    if (port::kv::read(port::kv::IdentityNamespace, name, out, size, &read) != MATTER_PORT_OK) {
        return false;
    }

    length = read;
    return true;
}

bool store_write(const char* name, const uint8_t* data, size_t size) {
    return port::kv::write(port::kv::IdentityNamespace, name, data, size) == MATTER_PORT_OK;
}

} // namespace identity
} // namespace matter
} // namespace espurna

// ---------------------------------------------------------------------------------------------------------------------
// The matter_port_* API of the library's platform layer

extern "C" {

namespace {

// the keys of the board's factory data (backend: identity::factory_read()) are read only, the ones Matter adds to
// "chip-factory" itself (uniqueId) go to the key-value store like every other namespace
bool factory_key(const char* ns, const char* name) {
    size_t length { 0 };
    return (std::strcmp(ns, espurna::matter::FactoryNamespace) == 0)
        && espurna::matter::identity::factory_read(name, nullptr, 0, length);
}

} // namespace

int matter_port_kv_read(const char* ns, const char* name, void* buf, size_t len, size_t* out_len) {
    using namespace espurna::matter;
    if (std::strcmp(ns, FactoryNamespace) == 0) {
        size_t length { 0 };
        if (identity::factory_read(name, static_cast<uint8_t*>(buf), len, length)) {
            if (out_len) {
                *out_len = std::min(len, length);
            }
            return MATTER_PORT_OK;
        }

        // Basic Information SerialNumber: factory data has none, Matter would report the SDK test value (matter.cpp:
        // the speaker hub reads it while commissioning)
        if (std::strcmp(name, "serial-num") == 0) {
            const auto* serial = matter_serial_number();
            const auto copy = std::min(len, std::strlen(serial));
            std::memcpy(buf, serial, copy);
            if (out_len) {
                *out_len = copy;
            }
            return MATTER_PORT_OK;
        }
    }

    return port::kv::read(ns, name, buf, len, out_len);
}

int matter_port_kv_write(const char* ns, const char* name, const void* buf, size_t len) {
    if (factory_key(ns, name)) {
        return MATTER_PORT_FAIL;
    }

    return espurna::matter::port::kv::write(ns, name, buf, len);
}

int matter_port_kv_remove(const char* ns, const char* name) {
    if (factory_key(ns, name)) {
        return MATTER_PORT_FAIL;
    }

    return espurna::matter::port::kv::remove(ns, name);
}

bool matter_port_kv_exists(const char* ns, const char* name) {
    return factory_key(ns, name) || espurna::matter::port::kv::exists(ns, name);
}

// only what is in the key-value store, never the factory data
int matter_port_kv_clear(const char* ns) {
    return espurna::matter::port::kv::clear(ns);
}

int matter_port_kv_erase_all(void) {
    return espurna::matter::port::kv::clear_all();
}

int matter_port_event_register(matter_port_event_module_t module, int event_id, matter_port_event_cb_t cb, void* arg) {
    espurna::matter::port::events::add(module, event_id, cb, arg);
    return MATTER_PORT_OK;
}

int matter_port_event_unregister(matter_port_event_module_t module, int, matter_port_event_cb_t cb) {
    espurna::matter::port::events::remove(module, cb);
    return MATTER_PORT_OK;
}

// Wi-Fi belongs to ESPurna. A controller that commissions over BLE (the matter_ble module) gives the network with
// Network Commissioning: matterWifiFromController() (matter.cpp); the library learns the result from the events
int matter_port_wifi_connect(const char* ssid, size_t ssid_len, const char* password, size_t password_len) {
    return matterWifiFromController(ssid, ssid_len, password, password_len) ? MATTER_PORT_OK : MATTER_PORT_FAIL;
}

int matter_port_wifi_link(matter_port_link_t* link) {
    std::memset(link, 0, sizeof(*link));
    if (!espurna::matter::port::events::up()) {
        return MATTER_PORT_FAIL;
    }

    const auto info = wifiStaInfo();
    std::strncpy(link->ssid, info.ssid.c_str(), sizeof(link->ssid) - 1);
    std::memcpy(link->bssid, info.bssid.data(), std::min(sizeof(link->bssid), info.bssid.size()));
    link->channel = info.channel;
    link->rssi = info.rssi;
    link->security = info.passphrase.length() ? MATTER_PORT_SECURITY_WPA2 : MATTER_PORT_SECURITY_NONE;

    return MATTER_PORT_OK;
}

void matter_port_wifi_mac(uint8_t mac[6]) {
    ::espurna::core::chip::wifi_mac(false, mac);
}

// the netif with the station's MAC (lwIP of the chip's SDK)
struct netif* matter_port_sta_netif(void) {
    uint8_t mac[6];
    ::espurna::core::chip::wifi_mac(false, mac);
    for (auto* netif = netif_list; netif; netif = netif->next) {
        if ((netif->hwaddr_len == sizeof(mac)) && (std::memcmp(netif->hwaddr, mac, sizeof(mac)) == 0)) {
            return netif;
        }
    }

    return nullptr;
}

int matter_port_wifi_scan_start(const char* ssid) {
    using namespace espurna::matter::port;
    {
        espurna::system::MutexLock lock(scan::mutex);
        scan::found.clear();
        scan::ssid = ssid ? espurna::string(ssid) : espurna::string();
    }

    return espurna::system::runInLoop([]() {
        if (!wifiScanStart(scan::on_found, scan::on_done)) {
            scan::on_done(espurna::string());
        }
    }) ? MATTER_PORT_OK : MATTER_PORT_FAIL;
}

int matter_port_wifi_scan_result(matter_port_scan_ap_t** aps, size_t* count) {
    using namespace espurna::matter::port;
    espurna::system::MutexLock lock(scan::mutex);
    *count = scan::found.size();
    *aps = nullptr;
    if (!*count) {
        return MATTER_PORT_OK;
    }

    *aps = static_cast<matter_port_scan_ap_t*>(std::malloc(sizeof(matter_port_scan_ap_t) * (*count)));
    if (!*aps) {
        *count = 0;
        return MATTER_PORT_FAIL;
    }

    std::copy(scan::found.begin(), scan::found.end(), *aps);
    return MATTER_PORT_OK;
}

void matter_port_wifi_scan_free(matter_port_scan_ap_t* aps) {
    std::free(aps);
}

// a factory reset of Matter or an update a controller sent: from the loop
void matter_port_reboot(void) {
    espurna::system::runInLoop([]() {
        prepareReset(CustomResetReason::Factory);
    });
}

void matter_port_log(const char* line) {
    matter_log(line);
}

size_t matter_port_heap_total(void) {
    return espurna::matter::backend::heap_total();
}

// ESPurna's own update path (ota_driver.h): the image the chip takes there, the same as /upgrade
int matter_port_ota_begin(const uint8_t* data, size_t len, size_t size) {
    return espurna::ota::driver::begin(data, len, size) ? MATTER_PORT_FAIL : MATTER_PORT_OK;
}

int matter_port_ota_write(const uint8_t* data, size_t len) {
    return espurna::ota::driver::write(data, len) ? MATTER_PORT_FAIL : MATTER_PORT_OK;
}

int matter_port_ota_end(void) {
    return espurna::ota::driver::end() ? MATTER_PORT_FAIL : MATTER_PORT_OK;
}

void matter_port_ota_abort(void) {
    espurna::ota::driver::abort();
}

} // extern "C"

#endif
