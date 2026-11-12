/*

MATTER FOR BEKEN (LibreTiny): WHAT ONLY THIS CHIP HAS

The Matter application (the C API of espurna_matter.h: matter_app_start() ...) is libMatterApp.a of matter-bk7231n
(connectedhomeip for the BK7231N), the rest of the Matter layer is matter.cpp. Here only what that library and this
chip need underneath: the bk_* platform API it was written against (matter_pal.h of the BK7231N port):
- "chip-factory" namespace: read only, straight from the factory data the stock firmware left in flash: the factory
  table of the Quectel SDK (Sparkleiot XH-CB2S), or the encrypted key-value store of Tuya's Matter firmware (MOES
  MS-104B Matter, CBU, ref. tuya_kv_beken.h): DAC and its key, PAI, certification declaration, passcode verifier,
  discriminator, VID / PID. Nothing is ever written there
- every other namespace (chip-config, chip-counters, the KVS of fabrics, ACL ...): FlashDB on the 'kvs' partition
- Wi-Fi status and the connected / got IP events from the ESPurna wifi module
- the store and the factory data of the board's identity (matter_identity.cpp, one code with the ESP32; the library
  calls matter_identity_install() before its server init)
- entropy from the TRNG of the chip
- the watchdog while a controller commissions, IGMP on the netif for the mDNS of Matter

*/

#include "espurna.h"

#if MATTER_SUPPORT

#include "espurna_matter.h"
#include "espurna_tasks.h"
#include "matter_identity.h"
#include "layout_beken.h"
#include "tuya_kv_beken.h"

#include <FreeRTOS.h>
#include <semphr.h>
#include <flashdb.h>

#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

#include <algorithm>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------------
// Types of the bk_* platform API of the Matter library (matter_pal.h of the BK7231N port)

extern "C" {

typedef int bk_err_t;

typedef enum {
    EVENT_MOD_WIFI,
    EVENT_MOD_NETIF,
} event_module_t;

typedef int (*event_cb_t)(void* arg, event_module_t event_module, int event_id, void* event_data);

typedef struct {
    int disconnect_reason;
    bool local_generated;
} matter_sta_disconnected_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int security;
    char password[65];
} wifi_sta_config_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    int security;
} wifi_link_status_t;

typedef struct {
    char ssid[33];
} wifi_scan_config_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int rssi;
    uint8_t channel;
    int security;
} wifi_scan_ap_info_t;

typedef struct {
    int ap_num;
    wifi_scan_ap_info_t* aps;
} wifi_scan_result_t;

} // extern "C"

namespace espurna {
namespace matter {
namespace {

constexpr int BkOk { 0 };
constexpr int BkFail { -1 };

constexpr int EventWifiStaConnected { 1 };
constexpr int EventWifiStaDisconnected { 2 };
constexpr int EventNetifGotIp4 { 0 };

constexpr char FactoryNamespace[] = "chip-factory";

// ---------------------------------------------------------------------------------------------------------------------

namespace factory {

// Namespace "chip-factory" of the stock key-value store. Table of 64-byte entries, each one
// {0x55 0xff 0xff 0xff, offset, length, name[32]}, values right after the table
// Factory data partition of the board, when its bootloader / layout has one (ref. layout_beken.cpp)
constexpr uint32_t TableStart { 0x7000 };
constexpr uint32_t TableSize { 0x1000 };
constexpr uint32_t DataStart { 0x8000 };
constexpr uint32_t DataSize { 0x2000 };
constexpr uint32_t EntryMagic { 0xffffff55 };

struct __attribute__((packed)) Entry {
    uint32_t magic;
    uint32_t offset;
    uint32_t length;
    char name[32];
    uint8_t reserved[20];
};

static_assert(sizeof(Entry) == 0x40, "");

// Copies at most size bytes, length is the full one
bool copy(const void* data, size_t data_length, uint8_t* out, size_t size, size_t& length) {
    length = data_length;
    const auto count = std::min(size, data_length);
    if (out && count) {
        std::memcpy(out, data, count);
    }

    return true;
}

// little endian, as the Quectel table keeps the numbers
bool copy_number(uint32_t value, size_t bytes, uint8_t* out, size_t size, size_t& length) {
    const uint8_t data[4] {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24)};
    return copy(data, bytes, out, size, length);
}

namespace tuya {

// Tuya's Matter firmware has no factory partition: the identity is in the encrypted key-value store of the Tuya SDK at
// the end of the flash, PEM and JSON (ref. tuya_kv_beken.h). Decrypted and converted once, in the loop (backend::prepare(),
// MATTER.FACTORY, the panel) before the Matter task asks for it, and kept: the library reads it at every
// commissioning step. Only on the Tuya layout, the store is out of reach of ESPurna's own partitions
using ::espurna::beken::tuya_kv::MatterFactory;

enum class State : uint8_t {
    Unknown,
    None,
    Found,
};

State state { State::Unknown };
MatterFactory* data { nullptr };

bool flash_read(uint32_t offset, uint8_t* out, size_t size) {
    return lt_flash_read(offset, out, size) == size;
}

const MatterFactory* get() {
    if (state != State::Unknown) {
        return data;
    }

    state = State::None;

    const auto& layout = ::espurna::beken::flashLayout();
    if ((layout.layout != ::espurna::beken::Layout::Tuya) || layout.matter_offset) {
        return nullptr;
    }

    // `matterTuya 0` keeps the board on its own identity even with Tuya's factory data on the chip
    if (!getSetting("matterTuya", true)) {
        return nullptr;
    }

    auto* out = new MatterFactory;
    if (!::espurna::beken::tuya_kv::load_matter_factory(flash_read, layout.flash_size, *out)) {
        delete out;
        return nullptr;
    }

    data = out;
    state = State::Found;
    DEBUG_MSG_P(PSTR("[MATTER] Factory identity in the Tuya key-value store at 0x%06X, VID 0x%04X PID 0x%04X\n"),
        out->offset, out->vendor_id, out->product_id);

    return data;
}

// Device type of the certification declaration (tag 3 of its TLV), 0 when there is none. The MOES MS-104B Matter
// board is certified as an On/Off Light (0x0100); a controller that knows the product by its VID/PID (the speaker hub)
// does not finish adding it while the endpoints say On/Off Plug-in Unit
uint16_t declared_device_type() {
    const auto* factory = get();
    if (!factory) {
        return 0;
    }

    const auto& cd = factory->declaration;

    // CMS SignedData: eContent of id-data (1.2.840.113549.1.7.1) is [0] { OCTET STRING { TLV } }
    constexpr uint8_t IdData[] { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01 };
    const auto it = std::search(cd.begin(), cd.end(), std::begin(IdData), std::end(IdData));
    if (it == cd.end()) {
        return 0;
    }

    size_t pos = (it - cd.begin()) + sizeof(IdData);
    const auto der_length = [&](size_t& at, size_t& out) -> bool {
        if (at >= cd.size()) {
            return false;
        }
        uint8_t first = cd[at++];
        if (first < 0x80) {
            out = first;
            return true;
        }
        const size_t bytes = first & 0x7f;
        if (!bytes || (bytes > 3) || ((at + bytes) > cd.size())) {
            return false;
        }
        out = 0;
        for (size_t index = 0; index < bytes; ++index) {
            out = (out << 8) | cd[at++];
        }
        return true;
    };

    size_t length;
    if ((pos >= cd.size()) || (cd[pos++] != 0xa0) || !der_length(pos, length)) {
        return 0;
    }
    if ((pos >= cd.size()) || (cd[pos++] != 0x04) || !der_length(pos, length) || ((pos + length) > cd.size())) {
        return 0;
    }

    // Matter TLV: anonymous structure, context tags, unsigned integers 0x04..0x07, strings 0x0c..0x13
    const size_t end = pos + length;
    if ((pos >= end) || (cd[pos++] != 0x15)) {
        return 0;
    }

    int depth = 0;
    while (pos < end) {
        const uint8_t control = cd[pos++];
        if (control == 0x18) {
            if (!depth) {
                break;
            }
            --depth;
            continue;
        }

        const uint8_t type = control & 0x1f;
        const uint8_t tag_form = control >> 5;
        int tag = -1;
        if (tag_form == 1) {
            if (pos >= end) {
                return 0;
            }
            tag = cd[pos++];
        } else if (tag_form != 0) {
            return 0;
        }

        if ((type >= 0x15) && (type <= 0x17)) {
            ++depth;
            continue;
        }

        if (type <= 0x07) {
            const size_t bytes = size_t(1) << (type & 0x03);
            if ((pos + bytes) > end) {
                return 0;
            }
            uint32_t value = 0;
            for (size_t index = 0; (index < bytes) && (index < 4); ++index) {
                value |= uint32_t(cd[pos + index]) << (8 * index);
            }
            pos += bytes;
            if (!depth && (tag == 3)) {
                return (value <= 0xffff) ? static_cast<uint16_t>(value) : 0;
            }
            continue;
        }

        if ((type == 0x08) || (type == 0x09) || (type == 0x14)) {
            continue;
        }

        if ((type >= 0x0c) && (type <= 0x13)) {
            const size_t bytes = size_t(1) << (type & 0x03);
            if ((pos + bytes) > end) {
                return 0;
            }
            size_t size = 0;
            for (size_t index = 0; (index < bytes) && (index < 4); ++index) {
                size |= size_t(cd[pos + index]) << (8 * index);
            }
            pos += bytes + size;
            continue;
        }

        return 0;
    }

    return 0;
}

bool read(const char* name, uint8_t* out, size_t size, size_t& length) {
    const auto* factory = get();
    if (!factory) {
        return false;
    }

    struct Bytes {
        const char* name;
        const void* data;
        size_t length;
    };

    const Bytes values[] {
        {"dac-cert", factory->dac.data(), factory->dac.size()},
        {"pai-cert", factory->pai.data(), factory->pai.size()},
        {"cert-dclrn", factory->declaration.data(), factory->declaration.size()},
        {"dac-pub-key", factory->dac_public.data(), factory->dac_public.size()},
        {"dac-key", factory->dac_private.data(), factory->dac_private.size()},
        {"salt", factory->salt.data(), factory->salt.size()},
        {"verifier", factory->verifier.data(), factory->verifier.size()},
        {"vendor-name", factory->vendor_name.data(), factory->vendor_name.size()},
        {"product-name", factory->product_name.data(), factory->product_name.size()},
        {"serial-num", factory->serial.data(), factory->serial.size()},
    };

    for (const auto& value : values) {
        if ((std::strcmp(name, value.name) == 0) && value.length) {
            return copy(value.data, value.length, out, size, length);
        }
    }

    struct Number {
        const char* name;
        uint32_t value;
        size_t bytes;
    };

    const Number numbers[] {
        {"discriminator", factory->discriminator, 2},
        {"vendor-id", factory->vendor_id, 2},
        {"product-id", factory->product_id, 2},
        {"hardware-ver", factory->hardware_version, 2},
        {"iteration-count", factory->iterations, 4},
        {"pin-code", factory->passcode, 4},
    };

    for (const auto& number : numbers) {
        if (std::strcmp(name, number.name) == 0) {
            return copy_number(number.value, number.bytes, out, size, length);
        }
    }

    return false;
}

} // namespace tuya

bool quectel_read(const char* name, uint8_t* out, size_t size, size_t& length);

// Value of a key, or false when there is none. Copies at most size bytes, length is the full one
bool read(const char* name, uint8_t* out, size_t size, size_t& length) {
    const auto& layout = ::espurna::beken::flashLayout();
    if (!layout.matter_offset) {
        // no factory data partition in this flash layout (Tuya 2 MiB boards): Tuya's Matter firmware may have left
        // the identity in its own store
        return tuya::read(name, out, size, length);
    }

    return quectel_read(name, out, size, length);
}

// Where the identity of the board comes from, for MATTER.FACTORY and the MATTER page. No secrets. Empty: no factory
// DAC, the board makes and uses its own development identity (matter_identity.cpp)
String source() {
    size_t length { 0 };
    if (!read("dac-cert", nullptr, 0, length)) {
        return String();
    }

    const auto& layout = ::espurna::beken::flashLayout();
    String out;
    if (layout.matter_offset) {
        out = F("Quectel factory table at 0x");
        out += String(layout.matter_offset, 16);
    } else {
        out = F("Tuya key-value store at 0x");
        out += String(tuya::data->offset, 16);
    }

    return out;
}

bool quectel_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    const auto& layout = ::espurna::beken::flashLayout();
    const uint32_t TableOffset { layout.matter_offset + TableStart };
    const uint32_t DataOffset { layout.matter_offset + DataStart };

    for (uint32_t offset = 0; offset < TableSize; offset += sizeof(Entry)) {
        Entry entry;
        if (lt_flash_read(TableOffset + offset, reinterpret_cast<uint8_t*>(&entry), sizeof(entry)) != sizeof(entry)) {
            return false;
        }

        if (entry.magic != EntryMagic) {
            continue;
        }

        if (std::strncmp(entry.name, name, sizeof(entry.name)) != 0) {
            continue;
        }

        // without overflow: a damaged entry must not wrap around to a small sum
        if ((entry.offset > DataSize) || (entry.length > (DataSize - entry.offset))) {
            return false;
        }

        length = entry.length;

        const auto copy = std::min(size, static_cast<size_t>(entry.length));
        return !copy || (lt_flash_read(DataOffset + entry.offset, out, copy) == copy);
    }

    return false;
}

bool read(const char* name, std::vector<uint8_t>& out) {
    size_t length { 0 };
    if (!read(name, nullptr, 0, length)) {
        return false;
    }

    out.resize(length);
    return read(name, out.data(), out.size(), length);
}

uint32_t read_number(const char* name) {
    uint8_t value[4] {};
    size_t length { 0 };
    if (!read(name, value, sizeof(value), length) || (length > sizeof(value))) {
        return 0;
    }

    uint32_t out { 0 };
    for (size_t index = length; index > 0; --index) {
        out = (out << 8) | value[index - 1];
    }

    return out;
}

String read_text(const char* name) {
    std::vector<uint8_t> value;
    if (!read(name, value)) {
        return String();
    }

    String out;
    out.concat(reinterpret_cast<const char*>(value.data()), value.size());

    return out;
}

} // namespace factory

// ---------------------------------------------------------------------------------------------------------------------

namespace kv {

// 'kvs' partition of the board (0x1D3000, 32 KiB), empty on the boards with Matter factory data
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

String key(const char* ns, const char* name) {
    String out;
    out.reserve(std::strlen(ns) + std::strlen(name) + 1);
    out += ns;
    out += '/';
    out += name;

    return out;
}

int read(const char* ns, const char* name, char* buf, uint32_t len, uint32_t* out_len) {
    if (!init()) {
        return BkFail;
    }

    struct fdb_kv obj;
    const auto full = key(ns, name);
    if (!fdb_kv_get_obj(&db, full.c_str(), &obj)) {
        return BkFail;
    }

    struct fdb_blob blob;
    const auto copy = std::min(len, static_cast<uint32_t>(obj.value_len));
    fdb_blob_read(reinterpret_cast<fdb_db_t>(&db), fdb_kv_to_blob(&obj, fdb_blob_make(&blob, buf, copy)));

    if (out_len) {
        *out_len = copy;
    }

    return BkOk;
}

int write(const char* ns, const char* name, const char* buf, uint32_t len) {
    if (!init()) {
        return BkFail;
    }

    struct fdb_blob blob;
    const auto full = key(ns, name);
    return (fdb_kv_set_blob(&db, full.c_str(), fdb_blob_make(&blob, buf, len)) == FDB_NO_ERR) ? BkOk : BkFail;
}

int remove(const char* ns, const char* name) {
    if (!init()) {
        return BkFail;
    }

    const auto full = key(ns, name);
    return (fdb_kv_del(&db, full.c_str()) == FDB_NO_ERR) ? BkOk : BkFail;
}

bool exists(const char* ns, const char* name) {
    if (!init()) {
        return false;
    }

    struct fdb_kv obj;
    const auto full = key(ns, name);
    return fdb_kv_get_obj(&db, full.c_str(), &obj) != nullptr;
}

int clear(const char* ns) {
    if (!init()) {
        return BkFail;
    }

    String prefix(ns);
    prefix += '/';

    std::vector<String> names;

    struct fdb_kv_iterator iterator;
    fdb_kv_iterator_init(&iterator);
    while (fdb_kv_iterate(&db, &iterator)) {
        if (std::strncmp(iterator.curr_kv.name, prefix.c_str(), prefix.length()) == 0) {
            names.emplace_back(iterator.curr_kv.name);
        }
    }

    for (const auto& name : names) {
        fdb_kv_del(&db, name.c_str());
    }

    return BkOk;
}

// Matter factory reset: fabrics, ACL, counters... everything but the own identity of the board (namespace
// "espurna-id" of matter_identity.cpp), so the pairing code stays the same
constexpr char IdentityPrefix[] = "espurna-id/";

int clear_all() {
    if (!init()) {
        return BkFail;
    }

    std::vector<String> names;

    struct fdb_kv_iterator iterator;
    fdb_kv_iterator_init(&iterator);
    while (fdb_kv_iterate(&db, &iterator)) {
        if (std::strncmp(iterator.curr_kv.name, IdentityPrefix, sizeof(IdentityPrefix) - 1) != 0) {
            names.emplace_back(iterator.curr_kv.name);
        }
    }

    for (const auto& name : names) {
        fdb_kv_del(&db, name.c_str());
    }

    return BkOk;
}

} // namespace kv

// ---------------------------------------------------------------------------------------------------------------------

// The library registers its network callbacks from the Matter task (add() / remove()), the loop reports the network
// to them (backend::network()): the list and the state are only touched under the mutex, the callbacks themselves run
// outside of it, from a copy (they may register again)
namespace events {

struct Callback {
    event_module_t module;
    event_cb_t callback;
    void* arg;
};

espurna::system::Mutex mutex;
std::vector<Callback> callbacks;
bool connected { false };

// the loop
void notify(event_module_t module, int id, void* data) {
    std::vector<Callback> copy;
    {
        espurna::system::MutexLock lock(mutex);
        copy = callbacks;
    }

    for (const auto& callback : copy) {
        if (callback.module == module) {
            callback.callback(callback.arg, module, id, data);
        }
    }
}

// the loop, before notify()
void network(bool up) {
    espurna::system::MutexLock lock(mutex);
    connected = up;
}

// the Matter task
void add(event_module_t module, event_cb_t callback, void* arg) {
    bool up;
    {
        espurna::system::MutexLock lock(mutex);
        for (auto& entry : callbacks) {
            if ((entry.module == module) && (entry.callback == callback)) {
                entry.arg = arg;
                return;
            }
        }

        callbacks.push_back(Callback{module, callback, arg});
        up = connected;
    }

    // Matter starts after ESPurna is already on the network
    if (up) {
        callback(arg, module,
            (module == EVENT_MOD_NETIF) ? EventNetifGotIp4 : EventWifiStaConnected,
            nullptr);
    }
}

void remove(event_module_t module, event_cb_t callback) {
    espurna::system::MutexLock lock(mutex);
    for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
        if (((*it).module == module) && ((*it).callback == callback)) {
            callbacks.erase(it);
            return;
        }
    }
}

} // namespace events

// ---------------------------------------------------------------------------------------------------------------------

namespace trng {

constexpr uintptr_t Ctrl { 0x00802480 };
constexpr uintptr_t Data { 0x00802484 };
constexpr uint32_t Enable { 1 };

void fill(unsigned char* out, size_t length) {
    auto* ctrl = reinterpret_cast<volatile uint32_t*>(Ctrl);
    const auto* data = reinterpret_cast<const volatile uint32_t*>(Data);

    *ctrl = *ctrl | Enable;

    while (length) {
        uint32_t word { 0 };
        for (size_t index = 0; index < 4; ++index) {
            delayMicroseconds(20);
            word = (word << 7) ^ (word >> 25) ^ *data;
        }

        const auto copy = std::min(length, sizeof(word));
        std::memcpy(out, &word, copy);
        out += copy;
        length -= copy;
    }

    *ctrl = *ctrl & ~Enable;
}

} // namespace trng

// The lwIP port of the BDK brings the netif up without NETIF_FLAG_IGMP (MLD6 is set), joining an IPv4 multicast
// group then fails with ERR_VAL, and the minimal mDNS of Matter cannot answer on 224.0.0.251. Runs in the tcpip thread
void enable_igmp(void*) {
    for (auto* netif = netif_list; netif; netif = netif->next) {
        if ((netif->flags & NETIF_FLAG_ETHARP) && !(netif->flags & NETIF_FLAG_IGMP)) {
            netif->flags |= NETIF_FLAG_IGMP;
            igmp_start(netif);
        }
    }
}

// device type of the certification declaration, read by prepare()
uint16_t declared_type { 0 };

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

namespace backend {

// On the 2 MiB boards the start right at boot, with both controllers reconnecting at once (CASE, subscriptions, reads)
// while DHCP, MQTT and the WebUI were busy too, filled the 16 KiB lwIP heap and receiving stopped: the board went deaf
// until the network watchdog restarted it, often twice in a row
uint32_t start_delay() {
    return 75;
}

void setup() {
}

// FlashDB of the library, and the Tuya factory identity decrypted here in the loop: the Matter task then only reads
// the copy
bool prepare() {
    if (!kv::init()) {
        return false;
    }

    factory::tuya::get();
    declared_type = factory::tuya::declared_device_type();

    return true;
}

uint16_t device_type() {
    return declared_type;
}

void network(bool up) {
    if (up) {
        events::network(true);
        tcpip_callback(enable_igmp, nullptr);
        events::notify(EVENT_MOD_WIFI, EventWifiStaConnected, nullptr);
        events::notify(EVENT_MOD_NETIF, EventNetifGotIp4, nullptr);
        return;
    }

    if (events::connected) {
        events::network(false);
        matter_sta_disconnected_t data {};
        events::notify(EVENT_MOD_WIFI, EventWifiStaDisconnected, &data);
    }
}

// The certificate steps of a pairing (attestation, CSR, AddNOC: ECDSA ~2 s per sign / verify on this chip) keep the
// higher priority Matter task busy longer than the 10 s the loop has to feed the watchdog: a 4-relay bridge reset at
// the hub's step 3. 30 s while a controller commissions, the loop value (system_beken.cpp LoopWdtTimeout) after it
void commissioning(bool active) {
    constexpr uint32_t LoopWdtMs { 10000 };
    constexpr uint32_t CommissioningWdtMs { 30000 };
    lt_wdt_enable(active ? CommissioningWdtMs : LoopWdtMs);
}

void describe(Identity& out) {
    const auto source = factory::source();
    if (!source.length()) {
        out.vendor = F("ESPurna");
        out.vendor_id = 0xfff1;
        out.product = F("ESPurna plug");
        out.product_id = 0x8000;
        out.source = F("own identity (development DAC made on the board), no factory data");
        return;
    }

    out.vendor = factory::read_text("vendor-name");
    out.vendor_id = factory::read_number("vendor-id");
    out.product = factory::read_text("product-name");
    out.product_id = factory::read_number("product-id");
    out.source = F("factory identity, ");
    out.source += source;

    // sizes only (a read without a buffer)
    size_t dac { 0 };
    size_t pai { 0 };
    size_t cd { 0 };
    factory::read("dac-cert", nullptr, 0, dac);
    factory::read("pai-cert", nullptr, 0, pai);
    factory::read("cert-dclrn", nullptr, 0, cd);

    out.details = F("certificates: DAC ");
    out.details += String(dac);
    out.details += F(", PAI ");
    out.details += String(pai);
    out.details += F(", declaration ");
    out.details += String(cd);
    out.details += F(" bytes");
}

// everything but the own identity of the board (namespace "espurna-id", matter_identity.cpp), so the pairing code stays
bool reset() {
    return kv::clear_all() == BkOk;
}

size_t heap_min() {
    return lt_heap_get_min_free();
}

} // namespace backend

// The chip's part of the identity (matter_identity.cpp): FlashDB of the 'kvs' partition, namespace "espurna-id" next
// to the stack's own; the factory data of the stock firmware, read only
namespace identity {
namespace {

constexpr char Namespace[] = "espurna-id";

} // namespace

bool store_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    uint32_t read { 0 };
    if (kv::read(Namespace, name, reinterpret_cast<char*>(out), size, &read) != BkOk) {
        return false;
    }

    length = read;
    return true;
}

bool store_write(const char* name, const uint8_t* data, size_t size) {
    return kv::write(Namespace, name, reinterpret_cast<const char*>(data), size) == BkOk;
}

bool factory_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    return factory::read(name, out, size, length);
}

} // namespace identity
} // namespace matter
} // namespace espurna

// The library calls it before its server init (matter-bk7231n matter_app.cpp): the providers of the board's identity,
// in the Matter task (the identity is made there on the first start, below the loop's priority)
extern "C" void matter_identity_install(void) {
    espurna::matter::identity::install();
}

// ---------------------------------------------------------------------------------------------------------------------
// The bk_* platform API of the Matter library

extern "C" {

// "chip-factory": the keys of the stock factory table are read only, the ones Matter adds there itself (uniqueId)
// go to the key-value store like every other namespace
int bk_read_data(const char* ns, const char* name, char* buf, uint32_t len, uint32_t* out_len) {
    using namespace espurna::matter;
    if (std::strcmp(ns, FactoryNamespace) == 0) {
        size_t length { 0 };
        if (factory::read(name, reinterpret_cast<uint8_t*>(buf), len, length)) {
            if (out_len) {
                *out_len = std::min(static_cast<size_t>(len), length);
            }
            return BkOk;
        }

        // Basic Information SerialNumber: the stock factory data has none, Matter then reports the SDK test value
        // (matter.cpp: the speaker hub reads it while commissioning)
        if (std::strcmp(name, "serial-num") == 0) {
            const auto* serial = matter_serial_number();
            const auto copy = std::min(static_cast<size_t>(len), std::strlen(serial));
            std::memcpy(buf, serial, copy);
            if (out_len) {
                *out_len = copy;
            }
            return BkOk;
        }
    }

    return kv::read(ns, name, buf, len, out_len);
}

namespace {

bool factory_key(const char* ns, const char* name) {
    using namespace espurna::matter;
    size_t length { 0 };
    return (std::strcmp(ns, FactoryNamespace) == 0) && factory::read(name, nullptr, 0, length);
}

} // namespace

int bk_write_data(const char* ns, const char* name, const char* buf, uint32_t len) {
    using namespace espurna::matter;
    if (factory_key(ns, name)) {
        return BkFail;
    }

    return kv::write(ns, name, buf, len);
}

int bk_clean_data(const char* ns, const char* name) {
    using namespace espurna::matter;
    if (factory_key(ns, name)) {
        return BkFail;
    }

    return kv::remove(ns, name);
}

int bk_ensure_namespace(const char*) {
    return espurna::matter::BkOk;
}

int bk_ensure_name_data(const char* ns, const char* name) {
    using namespace espurna::matter;
    if (factory_key(ns, name)) {
        return BkOk;
    }

    return kv::exists(ns, name) ? BkOk : BkFail;
}

// Clears only what is in the key-value store, never the stock factory table
int bK_clear_namespace(const char* ns) {
    return espurna::matter::kv::clear(ns);
}

int bk_erase_all(int) {
    return espurna::matter::kv::clear_all();
}

bk_err_t bk_event_register_cb(event_module_t module, int, event_cb_t cb, void* arg) {
    espurna::matter::events::add(module, cb, arg);
    return espurna::matter::BkOk;
}

bk_err_t bk_event_unregister_cb(event_module_t module, int, event_cb_t cb) {
    espurna::matter::events::remove(module, cb);
    return espurna::matter::BkOk;
}

// Wi-Fi belongs to ESPurna, Matter never connects on its own (on-network commissioning only)
bk_err_t bk_wifi_sta_set_config(const wifi_sta_config_t*) {
    return espurna::matter::BkFail;
}

bk_err_t bk_wifi_sta_start(void) {
    return espurna::matter::BkFail;
}

bk_err_t bk_wifi_sta_connect(void) {
    return espurna::matter::BkFail;
}

bk_err_t bk_wifi_sta_get_link_status(wifi_link_status_t* status) {
    std::memset(status, 0, sizeof(*status));
    if (!espurna::matter::events::connected) {
        return espurna::matter::BkFail;
    }

    const auto ssid = WiFi.SSID();
    std::strncpy(status->ssid, ssid.c_str(), sizeof(status->ssid) - 1);

    const auto* bssid = WiFi.BSSID();
    if (bssid) {
        std::memcpy(status->bssid, bssid, sizeof(status->bssid));
    }

    status->channel = WiFi.channel();
    status->rssi = WiFi.RSSI();

    return espurna::matter::BkOk;
}

bk_err_t bk_wifi_sta_get_mac(uint8_t* mac) {
    WiFi.macAddress(mac);
    return espurna::matter::BkOk;
}

bk_err_t bk_wifi_scan_start(const wifi_scan_config_t*) {
    return espurna::matter::BkFail;
}

bk_err_t bk_wifi_scan_get_result(wifi_scan_result_t* result) {
    result->ap_num = 0;
    result->aps = nullptr;
    return espurna::matter::BkFail;
}

void bk_wifi_scan_free_result(wifi_scan_result_t*) {
}

// No OTA through Matter, ESPurna updates itself
int bk_write_ota_data_to_flash(char*, uint32_t, uint32_t) {
    return espurna::matter::BkFail;
}

int bk_read_ota_data_in_flash(char*, uint32_t, uint32_t) {
    return espurna::matter::BkFail;
}

void bk_erase_ota_data_in_flash(void) {
}

size_t prvHeapGetTotalSize(void) {
    return lt_heap_get_size();
}

int mbedtls_hardware_poll(void*, unsigned char* output, size_t len, size_t* olen) {
    espurna::matter::trng::fill(output, len);
    *olen = len;
    return 0;
}

} // extern "C"


#endif
