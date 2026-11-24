/*

MATTER FOR BEKEN (BDK): WHAT ONLY THIS CHIP HAS

The Matter application and its platform layer are libMatterApp.a of matter-espurna, the same on every chip; the
platform layer's API (matter_port_*: key-value store, Wi-Fi, events, OTA) is matter_port.cpp, the rest of the Matter
layer matter.cpp. Here only what this chip has:
- the factory data the stock firmware left in flash ("chip-factory" of the key-value store, read only): the factory
  table of the Quectel SDK (Sparkleiot XH-CB2S), or the encrypted key-value store of Tuya's Matter firmware (MOES
  MS-104B Matter, CBU, ref. tuya_kv_beken.h): DAC and its key, PAI, certification declaration, passcode verifier,
  discriminator, VID / PID. Nothing is ever written there
- the watchdog while a controller commissions, IGMP on the netif for the mDNS (the lwIP port of the BDK), the heap

*/

#include "beken_api.h"
#include "../../espurna.h"

#if MATTER_SUPPORT

#include "../../espurna_matter.h"
#include "../../espurna_tasks.h"
#include "../../espurna_wifi.h"
#include "../../matter_identity.h"
#include "layout_beken.h"
#include "tuya_kv_beken.h"

#include <wifi_sdk.h>
#include <FreeRTOS.h>

#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>

// what the Matter library needs from the SDK of this firmware (FreeRTOS, lwIP, mbedTLS, the layout of the types it
// shares with it): a mismatch stops the build here. Libraries before that check have no such header
#if __has_include(<app/matter_requirements.h>)
#include <app/matter_requirements.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>


namespace espurna {
namespace matter {
namespace {

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
    return ::espurna::beken::flash_read(offset, out, size) == size;
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
espurna::string source() {
    size_t length { 0 };
    if (!read("dac-cert", nullptr, 0, length)) {
        return espurna::string();
    }

    const auto& layout = ::espurna::beken::flashLayout();
    espurna::string out;
    if (layout.matter_offset) {
        out = "Quectel factory table at 0x";
        out += espurna::string(layout.matter_offset, 16);
    } else {
        out = "Tuya key-value store at 0x";
        out += espurna::string(tuya::data->offset, 16);
    }

    return out;
}

bool quectel_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    const auto& layout = ::espurna::beken::flashLayout();
    const uint32_t TableOffset { layout.matter_offset + TableStart };
    const uint32_t DataOffset { layout.matter_offset + DataStart };

    for (uint32_t offset = 0; offset < TableSize; offset += sizeof(Entry)) {
        Entry entry;
        if (::espurna::beken::flash_read(TableOffset + offset, reinterpret_cast<uint8_t*>(&entry), sizeof(entry)) != sizeof(entry)) {
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
        return !copy || (::espurna::beken::flash_read(DataOffset + entry.offset, out, copy) == copy);
    }

    return false;
}

} // namespace factory

// ---------------------------------------------------------------------------------------------------------------------

// The lwIP port of the BDK brings the netif up without NETIF_FLAG_IGMP (MLD6 is set), joining an IPv4 multicast
// group then fails with ERR_VAL, and mDNS cannot answer on 224.0.0.251. Runs in the tcpip thread
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

// the Tuya factory identity decrypted here in the loop: the Matter task then only reads the copy
bool prepare() {
    factory::tuya::get();
    declared_type = factory::tuya::declared_device_type();

    return true;
}

uint16_t device_type() {
    return declared_type;
}

void network(bool up) {
    if (up) {
        tcpip_callback(enable_igmp, nullptr);
    }
}

size_t heap_total() {
    return ::espurna::beken::heap_size();
}

} // namespace backend

// The chip's part of the identity (matter_identity.cpp): the factory data of the stock firmware, read only
namespace identity {

bool factory_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    return factory::read(name, out, size, length);
}

espurna::string factory_source() {
    return factory::source();
}

} // namespace identity
} // namespace matter
} // namespace espurna

#endif
