/*

MATTER, WHAT ONLY THE ESP32 HAS (Arduino-ESP32 core 3.x, ESP-IDF 5.5)

The Matter application is libMatterApp.a of matter-espurna, the same as on the BK7231N (connectedhomeip, the plug.zap
data model, the C API of espurna_matter.h), on connectedhomeip's ESP32 platform (scripts/matter_lib.py); the rest of the
Matter layer is matter.cpp, matter_app.cpp and matter_identity.cpp. Here only what the chip's SDK forces: IPv6 of the
station, the factory reset of the ESP32 key-value store (NVS), the heap, the store of the board's identity (NVS).
Commissioning is on-network only: ESPurna is on Wi-Fi already, the library is built without CHIPoBLE

*/

#include "espurna.h"

#if MATTER_SUPPORT

#include "espurna_matter.h"
#include "matter_identity.h"

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/KeyValueStoreManager.h>

#include <nvs.h>

#include <cstring>

namespace espurna {
namespace matter {
namespace backend {

// starts right away
uint32_t start_delay() {
    return 0;
}

void setup() {
    // Matter runs over IPv6 (link-local at least): through the common Wi-Fi, which also adds the address at once when
    // the station is connected already (the Matter module starts after wifiSetup())
    wifiIpv6(true);
}

// NVS is up since the boot, the identity is made in the server init of the library (matter_identity_install())
bool prepare() {
    return true;
}

uint16_t device_type() {
    return 0;
}

void network(bool) {
}

void commissioning(bool) {
}

void describe(Identity& out) {
    uint16_t vendor { 0 };
    uint16_t product { 0 };
    // the providers are only set up by the server init: before that (the WebUI asks on every connection) it crashed
    if (matterStarted()) {
        auto* info = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
        if (info) {
            info->GetVendorId(vendor);
            info->GetProductId(product);
        }
    } else {
        // before the start: the ones of the chosen identity (`matterIdentity`, matter_identity.cpp)
        espurna::matter::identity::ids(vendor, product);
    }

    out.vendor = F("ESPurna");
    out.vendor_id = vendor;
    out.product = systemDevice().toString();
    out.product_id = product;
    out.source = espurna::matter::identity::status();
}

// What a Matter factory reset clears: chip-config (fabric index, unique id ...) and the KVS of fabrics, ACL, sessions.
// The counters stay (no reuse of a message counter), the own identity stays in its "espurna-id" namespace
bool reset() {
    using namespace chip::DeviceLayer::Internal;
    using chip::DeviceLayer::PersistedStorage::KeyValueStoreMgrImpl;

    const auto config = ESP32Config::ClearNamespace(ESP32Config::kConfigNamespace_ChipConfig);
    const auto kvs = KeyValueStoreMgrImpl().EraseAll();

    return (config == CHIP_NO_ERROR) && (kvs == CHIP_NO_ERROR);
}

} // namespace backend

// The chip's part of the identity (matter_identity.cpp): NVS of the default 'nvs' partition, namespace "espurna-id"
// next to the stack's own (CHIP_KVS, chip-config, chip-counters); no factory data on any ESP32
namespace identity {
namespace {

constexpr char Namespace[] = "espurna-id";

} // namespace

bool store_read(const char* name, uint8_t* out, size_t size, size_t& length) {
    nvs_handle_t handle;
    if (nvs_open(Namespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    // a read without a buffer: the size only
    size_t blob { out ? size : 0 };
    const auto err = nvs_get_blob(handle, name, out, &blob);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    length = blob;
    return true;
}

bool store_write(const char* name, const uint8_t* data, size_t size) {
    nvs_handle_t handle;
    if (nvs_open(Namespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    auto err = nvs_set_blob(handle, name, data, size);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK;
}

bool factory_read(const char*, uint8_t*, size_t, size_t&) {
    return false;
}

} // namespace identity
} // namespace matter
} // namespace espurna

#endif
