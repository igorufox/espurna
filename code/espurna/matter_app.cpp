/*

MATTER: THE PART OF THE MATTER APPLICATION THAT IS THE SAME ON EVERY CHIP (the C API of espurna_matter.h)

The pairing codes (from the providers of the board's identity, matter_identity.cpp), the controllers the device is
added to (the fabric table: list, removal), the pairing window, the liveness check of the Matter task: the server of
connectedhomeip of libMatterApp.a of matter-espurna, the same on ESP32 and Beken.

The start, the plug endpoints and their On/Off attribute (matter_app_start(), matter_app_plugs(), matter_set_onoff())
are the library's application, the same plug.zap data model on both chips

*/

#include "espurna.h"

#if MATTER_SUPPORT

#include "espurna_matter.h"

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace espurna {
namespace matter {
namespace app {
namespace {

using namespace ::chip;
using namespace ::chip::DeviceLayer;

void copy(const std::string& value, char* out, size_t size) {
    const auto length = std::min(value.size(), size - 1);
    std::memcpy(out, value.data(), length);
    out[length] = '\0';
}

// the payload of the codes: the identity's passcode and discriminator, VID / PID; on-network commissioning only
bool payload(SetupPayload& out) {
    uint32_t passcode { 0 };
    uint16_t discriminator { 0 };
    uint16_t vendor { 0 };
    uint16_t product { 0 };

    auto* commissionable = GetCommissionableDataProvider();
    auto* info = GetDeviceInstanceInfoProvider();
    if (!commissionable || !info
        || (commissionable->GetSetupPasscode(passcode) != CHIP_NO_ERROR)
        || (commissionable->GetSetupDiscriminator(discriminator) != CHIP_NO_ERROR)
        || (info->GetVendorId(vendor) != CHIP_NO_ERROR)
        || (info->GetProductId(product) != CHIP_NO_ERROR))
    {
        return false;
    }

    out.version = 0;
    out.vendorID = vendor;
    out.productID = product;
    out.commissioningFlow = CommissioningFlow::kStandard;
    out.rendezvousInformation.SetValue(RendezvousInformationFlags(RendezvousInformationFlag::kOnNetwork));
    out.discriminator.SetLongValue(discriminator);
    out.setUpPINCode = passcode;

    return true;
}

// The fabric table as last read. The loop only tries the CHIP stack lock: it does not wait while the Matter task holds
// it for a certificate step of a commissioning (seconds on the BK7231N), the list of the reading before stays then
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
    if (!matterStarted() || !PlatformMgr().TryLockChipStack()) {
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

    PlatformMgr().UnlockChipStack();
}

} // namespace
} // namespace app
} // namespace matter
} // namespace espurna

extern "C" {

// Manual pairing code and QR payload ("MT:..."). They carry the setup passcode: only for the device owner
bool matter_onboarding_codes(char* manual, size_t manual_size, char* qr, size_t qr_size) {
    using namespace espurna::matter::app;

    if (!matterStarted() || !manual_size || !qr_size) {
        return false;
    }

    chip::SetupPayload out;
    if (!payload(out)) {
        return false;
    }

    std::string text;
    if (chip::ManualSetupPayloadGenerator(out).payloadDecimalStringRepresentation(text) != CHIP_NO_ERROR) {
        return false;
    }
    copy(text, manual, manual_size);

    text.clear();
    if (chip::QRCodeSetupPayloadGenerator(out).payloadBase38Representation(text) != CHIP_NO_ERROR) {
        return false;
    }
    copy(text, qr, qr_size);

    return true;
}

int matter_fabric_count(void) {
    espurna::matter::app::refresh_fabrics();
    return static_cast<int>(espurna::matter::app::fabric_count);
}

// Controllers the device was added to: the n-th one (0..), false after the last. n = 0 reads the table again, the rest
// of a list comes from the same reading; label gets the fabric label, cut to label_size - 1
bool matter_fabric_info(int n, uint8_t* index, uint16_t* vendor, uint64_t* node, char* label, size_t label_size) {
    using namespace espurna::matter::app;

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

// The device leaves a controller, like on its RemoveFabric command (the server drops the sessions, access entries and
// group keys of the fabric, and opens the pairing window when it was the last one). In the Matter task
bool matter_remove_fabric(uint8_t index) {
    if (!matterStarted()) {
        return false;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    const bool found = chip::Server::GetInstance().GetFabricTable().FindFabricWithIndex(index) != nullptr;
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    if (!found) {
        return false;
    }

    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            const auto fabric = static_cast<chip::FabricIndex>(arg);
            const auto err = chip::Server::GetInstance().GetFabricTable().Delete(fabric);
            if (err != CHIP_NO_ERROR) {
                ChipLogError(AppServer, "Removing fabric %u failed: %" CHIP_ERROR_FORMAT,
                    static_cast<unsigned>(fabric), err.Format());
            }
        },
        static_cast<intptr_t>(index));

    return true;
}

// The pairing window again (it closes after 15 minutes, or once the device is commissioned). In the Matter task
void matter_open_window(void) {
    if (!matterStarted()) {
        return;
    }

    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t) {
            auto& manager = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!manager.IsCommissioningWindowOpen()) {
                manager.OpenBasicCommissioningWindow();
            }
        },
        0);
}

// Liveness check of the Matter task: a work item that calls back into ESPurna
void matter_ping(void) {
    if (matterStarted()) {
        chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) { matter_pong(); }, 0);
    }
}

} // extern "C"

#endif // MATTER_SUPPORT
