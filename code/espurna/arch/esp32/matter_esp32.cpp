/*

MATTER APPLICATION FOR ESP32 (Arduino-ESP32 core 3.x)

The Matter application C API of espurna_matter.h on the Matter library of Arduino-ESP32 (esp_matter, connectedhomeip):
On/Off plug endpoints, a bridge (Aggregator + Bridged Nodes) for more than one relay. Everything else is matter.cpp.
Commissioning is on-network only: ESPurna is on Wi-Fi already, and the libraries are built without CHIPoBLE. Setup
codes and device attestation are the board's own (matter_identity_esp32.cpp), under the SDK test PAI (VID 0xFFF1,
PID 0x8000)

*/

#include "espurna.h"

#if defined(ARDUINO_ARCH_ESP32) && MATTER_SUPPORT

#include "espurna_matter.h"
#include "matter_identity_esp32.h"

#include <Matter.h>
#include <WiFi.h>

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Config.h>
#include <platform/KeyValueStoreManager.h>

#include <cstring>

namespace espurna {
namespace matter {
namespace backend {
namespace {

constexpr size_t MaxPlugs { 4 };

MatterOnOffPlugin plugs[MaxPlugs];
size_t count { 0 };
bool begun { false };

// More than one relay: the device is a bridge (gateway). Every relay plug becomes a Bridged Node with its own name
// under an Aggregator endpoint, so controllers show one gateway with a plug per relay (the speaker hub showed a single
// plug for a device with two plug endpoints). The usual bridge layout: Aggregator on endpoint 1, the plugs after it
// (the hub showed the gateway without plugs when the Aggregator came after them)
struct BridgedName {
    char label[esp_matter::cluster::bridged_device_basic_information::k_max_node_label_length + 1];
    char unique[esp_matter::cluster::bridged_device_basic_information::k_max_unique_id_length + 1];
};

BridgedName names[MaxPlugs];
bool bridge { false };

// A char string attribute of a cluster: the value of the existing one, or a new one
void set_text(esp_matter::cluster_t* cluster, uint32_t id, char* value,
        esp_matter::attribute_t* (*create)(esp_matter::cluster_t*, char*, uint16_t)) {
    const auto length = static_cast<uint16_t>(std::strlen(value));
    auto* attribute = esp_matter::attribute::get(cluster, id);
    if (!attribute) {
        create(cluster, value, length);
        return;
    }

    auto val = esp_matter_char_str(value, length);
    esp_matter::attribute::set_val(attribute, &val, false);
}

// The node of the Arduino Matter library is made by its first endpoint; the Aggregator comes before the plugs
struct MatterNode : public ArduinoMatter {
    static void init() {
        _init();
    }
};

esp_matter::endpoint_t* make_aggregator() {
    using namespace esp_matter;

    MatterNode::init();
    auto* node = node::get();
    if (!node) {
        return nullptr;
    }

    endpoint::aggregator::config_t config;
    return endpoint::aggregator::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
}

bool make_bridge(esp_matter::endpoint_t* aggregator) {
    using namespace esp_matter;

    auto* node = node::get();
    if (!node || !aggregator) {
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        auto* plug = endpoint::get(node, plugs[index].getEndPointId());
        if (!plug) {
            return false;
        }

        endpoint::bridged_node::config_t config;
        if (endpoint::bridged_node::add(plug, &config) != ESP_OK) {
            return false;
        }

        auto* info = cluster::get(plug, chip::app::Clusters::BridgedDeviceBasicInformation::Id);
        if (!info) {
            return false;
        }

        // UniqueID is mandatory, bridged_node::add() creates it empty: set the values of existing attributes
        matter_plug_name(static_cast<uint8_t>(index + 1), names[index].label, sizeof(names[index].label),
            names[index].unique, sizeof(names[index].unique));
        set_text(info, chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
            names[index].label, cluster::bridged_device_basic_information::attribute::create_node_label);
        set_text(info, chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::UniqueID::Id,
            names[index].unique, cluster::bridged_device_basic_information::attribute::create_unique_id);

        endpoint::set_parent_endpoint(plug, aggregator);
    }

    DEBUG_MSG_P(PSTR("[MATTER] Bridge: aggregator endpoint %u, %u bridged plug(s)\n"),
        endpoint::get_id(aggregator), count);

    return true;
}

// "https://...qrcode.html?data=MT%3A..." -> "MT:..."
bool qr_payload(const String& url, char* out, size_t size) {
    const auto start = url.indexOf(F("data="));
    if ((start < 0) || !size) {
        return false;
    }

    size_t length { 0 };
    for (size_t index = start + 5; (index < url.length()) && (length + 1 < size); ++index) {
        char c = url[index];
        if ((c == '%') && (index + 2 < url.length())) {
            const char hex[3] { url[index + 1], url[index + 2], '\0' };
            c = static_cast<char>(std::strtoul(hex, nullptr, 16));
            index += 2;
        }
        out[length++] = c;
    }
    out[length] = '\0';

    return std::strncmp(out, "MT:", 3) == 0;
}

// The fabric table as last read. The loop only tries the CHIP stack lock: it does not wait while the Matter task holds
// it for a certificate step of a commissioning, the list of the attempt before stays then
struct Fabric {
    uint8_t index;
    uint16_t vendor;
    uint64_t node;
    char label[33];
};

constexpr size_t MaxFabrics { 8 };
Fabric fabrics[MaxFabrics];
size_t fabric_count { 0 };

void refresh_fabrics() {
    if (!begun || !chip::DeviceLayer::PlatformMgr().TryLockChipStack()) {
        return;
    }

    size_t out { 0 };
    for (const auto& fabric : chip::Server::GetInstance().GetFabricTable()) {
        if (out >= MaxFabrics) {
            break;
        }

        auto& entry = fabrics[out++];
        entry.index = fabric.GetFabricIndex();
        entry.vendor = static_cast<uint16_t>(fabric.GetVendorId());
        entry.node = fabric.GetNodeId();

        const auto label = fabric.GetFabricLabel();
        const auto length = std::min(label.size(), sizeof(entry.label) - 1);
        std::memcpy(entry.label, label.data(), length);
        entry.label[length] = '\0';
    }
    fabric_count = out;

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

} // namespace

// starts right away
uint32_t start_delay() {
    return 0;
}

void setup() {
    // Matter runs over IPv6 (link-local at least): through the common Wi-Fi, which also adds the address at once when
    // the station is connected already (the Matter module starts after wifiSetup())
    wifiIpv6(true);
}

// NVS is up since the boot, the identity is made in matter_app_start()
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
    // the provider is only set up by Matter.begin(): before that (the WebUI asks on every connection) it crashed
    if (begun) {
        auto* info = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
        if (info) {
            info->GetVendorId(vendor);
            info->GetProductId(product);
        }
    }

    out.vendor = F("ESPurna");
    out.vendor_id = vendor;
    out.product = systemDevice().toString();
    out.product_id = product;
    out.source = identity::status();
}

// What a Matter factory reset clears: chip-config (fabric index, unique id ...) and the KVS of fabrics, ACL, sessions.
// The counters stay (no reuse of a message counter), the own identity stays in its "espurna-matter" namespace
bool reset() {
    using namespace chip::DeviceLayer::Internal;
    using chip::DeviceLayer::PersistedStorage::KeyValueStoreMgrImpl;

    const auto config = ESP32Config::ClearNamespace(ESP32Config::kConfigNamespace_ChipConfig);
    const auto kvs = KeyValueStoreMgrImpl().EraseAll();

    return (config == CHIP_NO_ERROR) && (kvs == CHIP_NO_ERROR);
}

size_t heap_min() {
    return esp_get_minimum_free_heap_size();
}

} // namespace backend
} // namespace matter
} // namespace espurna

// ---------------------------------------------------------------------------------------------------------------------
// The Matter application C API (espurna_matter.h)

using namespace espurna::matter::backend;

extern "C" {

uint8_t matter_app_plugs(void) {
    return MaxPlugs;
}

int matter_app_start(void) {
    if (begun) {
        return 0;
    }

    // the endpoints exist after the first try, a second one would add them again
    static bool tried { false };
    if (tried) {
        return -1;
    }
    tried = true;

    count = std::min(static_cast<size_t>(matter_plug_count()), MaxPlugs);

    // before Matter.begin(): the data model is fixed once the stack runs
    esp_matter::endpoint_t* aggregator { nullptr };
    if (count > 1) {
        aggregator = make_aggregator();
    }

    for (size_t index = 0; index < count; ++index) {
        plugs[index].begin(false);
        // Matter task: the relay is switched in the loop
        plugs[index].onChange([index](bool value) {
            matter_onoff_changed(static_cast<uint8_t>(index + 1), value);
            return true;
        });
    }

    if (count > 1) {
        bridge = make_bridge(aggregator);
        if (!bridge) {
            DEBUG_MSG_P(PSTR("[MATTER] Bridge setup failed, plain plug endpoints\n"));
        }
    }

    // Basic Information (the setters copy)
    Matter.setVendorName("ESPurna");
    Matter.setProductName(matter_product_label());
    Matter.setDeviceName(matter_node_label());
    Matter.setSerialNumber(matter_serial_number());
    Matter.setHardwareVersion(1);
    Matter.setHardwareVersionString("1");

    Matter.onEvent([](matterEvent_t event, const chip::DeviceLayer::ChipDeviceEvent*) {
        switch (event) {
        case MATTER_COMMISSIONING_SESSION_STARTED:
            matter_commissioning(true);
            break;
        case MATTER_COMMISSIONING_SESSION_STOPPED:
        case MATTER_COMMISSIONING_COMPLETE:
        case MATTER_FAIL_SAFE_TIMER_EXPIRED:
            matter_commissioning(false);
            break;
        case MATTER_COMMISSIONING_WINDOW_OPEN:
            matter_window(true);
            break;
        case MATTER_COMMISSIONING_WINDOW_CLOSED:
            matter_window(false);
            break;
        default:
            break;
        }
    });

    // the SDK test codes stay when NVS fails
    uint32_t passcode;
    uint16_t discriminator;
    if (identity::load(passcode, discriminator)) {
        Matter.setSetupPasscode(passcode);
        Matter.setSetupDiscriminator(discriminator);
    }

    Matter.begin();
    if (!esp_matter::is_started()) {
        return -1;
    }
    begun = true;

    // NodeLabel is kept in NVS by the stack, the value of the endpoint before (another relay, an older name) would stay:
    // the ESPurna relay name is the one
    if (bridge) {
        for (size_t index = 0; index < count; ++index) {
            auto val = esp_matter_char_str(names[index].label, std::strlen(names[index].label));
            esp_matter::attribute::update(plugs[index].getEndPointId(),
                chip::app::Clusters::BridgedDeviceBasicInformation::Id,
                chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, &val);
        }
    }

    // first start: the key and the certificate take a moment (two signatures). The provider is only there after
    // Matter.begin(), and the Matter task may already read it (a controller commissions right away): it is swapped
    // with the CHIP stack lock held
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const bool installed = identity::install();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (!installed) {
        DEBUG_MSG_P(PSTR("[MATTER] Own device attestation failed, the SDK test one stays\n"));
    }
    DEBUG_MSG_P(PSTR("[MATTER] Identity: %s\n"), identity::status().c_str());

    return 0;
}

// The attribute itself, not MatterOnOffPlugin::setOnOff(): that one compares with its own copy of the state, while the
// stack loads the value it kept in NVS (the relay state stayed unsent to controllers then, e.g. after the endpoints
// were renumbered). matter.cpp sends every relay state right after the start
void matter_set_onoff(uint8_t endpoint, bool on) {
    if (!begun || (endpoint < 1) || (endpoint > count)) {
        return;
    }

    using namespace chip::app::Clusters;
    const auto id = plugs[endpoint - 1].getEndPointId();

    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    if ((esp_matter::attribute::get_val(id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val) == ESP_OK)
        && (val.val.b == on)) {
        return;
    }

    val = esp_matter_bool(on);
    esp_matter::attribute::update(id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

bool matter_onboarding_codes(char* manual, size_t manual_size, char* qr, size_t qr_size) {
    if (!begun || !manual_size || !qr_size) {
        return false;
    }

    const auto code = Matter.getManualPairingCode();
    if (!code.length()) {
        return false;
    }

    std::strncpy(manual, code.c_str(), manual_size - 1);
    manual[manual_size - 1] = '\0';

    return qr_payload(Matter.getOnboardingQRCodeUrl(), qr, qr_size);
}

int matter_fabric_count(void) {
    refresh_fabrics();
    return static_cast<int>(fabric_count);
}

// n = 0 reads the table again, the rest of a list comes from the same reading
bool matter_fabric_info(int n, uint8_t* index, uint16_t* vendor, uint64_t* node, char* label, size_t label_size) {
    if (n == 0) {
        refresh_fabrics();
    }

    if ((n < 0) || (static_cast<size_t>(n) >= fabric_count) || !label_size) {
        return false;
    }

    const auto& entry = fabrics[n];
    *index = entry.index;
    *vendor = entry.vendor;
    *node = entry.node;
    std::strncpy(label, entry.label, label_size - 1);
    label[label_size - 1] = '\0';

    return true;
}

// What the RemoveFabric command of a controller does: the server drops the sessions, access entries and group keys of
// that fabric with it, and opens the pairing window when it was the last one
bool matter_remove_fabric(uint8_t index) {
    if (!begun) {
        return false;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto& table = chip::Server::GetInstance().GetFabricTable();
    const bool found = table.FindFabricWithIndex(index) != nullptr;
    if (found) {
        const auto err = table.Delete(index);
        if (err != CHIP_NO_ERROR) {
            DEBUG_MSG_P(PSTR("[MATTER] Removing controller #%u failed: %s\n"), index, chip::ErrorStr(err));
        }
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    return found;
}

void matter_open_window(void) {
    if (!begun) {
        return;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    auto& manager = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (!manager.IsCommissioningWindowOpen()) {
        manager.OpenBasicCommissioningWindow();
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

void matter_ping(void) {
    if (begun) {
        chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) { matter_pong(); });
    }
}

} // extern "C"

#endif
