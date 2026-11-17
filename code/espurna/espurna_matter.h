/*

MATTER MODULE

(espurna_matter.h, not matter.h: on a case-insensitive file system <Matter.h> of Arduino-ESP32 found this one)

Matter On/Off plug endpoints for the relays. One Matter layer for both chips:
- matter.cpp: everything the chips share. Settings, the start (`matterAuto` / `matterDelay`), the relay link both ways,
  the plug names of a bridge, the controller (fabric) list and removal, the pairing window, the pairing codes, the quiet
  mode while a controller commissions, the log of the Matter task, the terminal commands and the MATTER panel
- the Matter application C API below (`matter_app_*`, `matter_*`): the same functions on both chips, matter.cpp knows
  only them. The start, the plug endpoints and their On/Off attribute (`matter_app_start`, `matter_app_plugs`,
  `matter_set_onoff`): libMatterApp.a of matter-espurna on both chips, the same connectedhomeip, plug.zap data model and
  application, the platform of connectedhomeip of the chip under it. The pairing codes, the controllers, the pairing
  window, the ping (matter_app.cpp) and the identity of the board (matter_identity.cpp): one code of ESPurna on the
  server API
- the callbacks of that API (`matter_onoff_changed`, `matter_commissioning` ...): defined once, in matter.cpp
- backend::* below: what the SDK of the chip forces and nothing else (Beken: the bk_* platform API, the factory data,
  FlashDB key-value store, watchdog; ESP32: IPv6 on, the identity's store in NVS)

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------------------------------------------------
// The Matter application (the chip's implementation). Called from the loop. `endpoint` is the plug endpoint, relay + 1

extern "C" {

// 0 = the stack runs
int matter_app_start(void);
// plug endpoints the data model can have (1..4). Weak: Beken libraries before multi-plug have only endpoint 1
uint8_t matter_app_plugs(void) __attribute__((weak));
void matter_set_onoff(uint8_t endpoint, bool on);
bool matter_onboarding_codes(char* manual, size_t manual_size, char* qr, size_t qr_size);
int matter_fabric_count(void);
// controller n (0..) of the fabric table; false after the last one. Weak: older Beken libraries do not have these
bool matter_fabric_info(int n, uint8_t* index, uint16_t* vendor, uint64_t* node, char* label, size_t label_size)
    __attribute__((weak));
// what the RemoveFabric command of a controller does
bool matter_remove_fabric(uint8_t index) __attribute__((weak));
// the basic pairing window, when none is open
void matter_open_window(void);
// a work item through the Matter task, answered with matter_pong()
void matter_ping(void);
// after matter_app_start(): the last controller gone, the basic pairing window opens again (matter_app.cpp)
void matter_watch_fabrics(void);

// ---------------------------------------------------------------------------------------------------------------------
// Callbacks of the Matter application (matter.cpp). From the Matter task unless said otherwise: applied in the loop

void matter_onoff_changed(uint8_t endpoint, bool on);
void matter_commissioning(bool active);
void matter_window(bool open);
void matter_pong(void);
// one log line of the Matter stack
void matter_log(const char* line);

// Filled in the loop before matter_app_start(), constant while Matter runs (settings are not read from the Matter task)
const char* matter_node_label(void);
const char* matter_serial_number(void);
const char* matter_product_label(void);
// plug endpoints in use (the relays)
uint8_t matter_plug_count(void);
// device type of the plug endpoints, 0 = On/Off Plug-in Unit
uint16_t matter_device_type(void);
// bridged plug name (relay name `relayName<n>`, else "<hostname> <n>") and unique id ("<chip id>-<n>"), plug = relay + 1
bool matter_plug_name(uint8_t plug, char* label, size_t label_size, char* unique, size_t unique_size);

} // extern "C"

// ---------------------------------------------------------------------------------------------------------------------

namespace espurna {
namespace matter {
namespace backend {

// What only the chip has, called from the loop

// Default `matterDelay`, seconds between the station address and the automatic start
uint32_t start_delay();

void setup();

// Before matter_app_start(): storage and identity of the chip. false: Matter cannot start
bool prepare();

// Device type of the plug endpoints the identity of the board asks for (0 = plug), after prepare()
uint16_t device_type();

// Station got / lost its address
void network(bool up);

// From the Matter task (matter_commissioning()): what the chip needs while a controller commissions
void commissioning(bool active);

// Where the device attestation comes from, no secrets
struct Identity {
    String vendor;
    uint16_t vendor_id { 0 };
    String product;
    uint16_t product_id { 0 };
    String source;
    String details;
};

void describe(Identity& out);

// Forget every controller (fabrics, ACL), keep the own identity and its pairing code. Only while Matter is stopped
bool reset();

} // namespace backend

// Heap checkpoint of the start (W88): "[MATTER] heap <step>: free F, block B, min M" - free heap, its largest free
// block, the lowest free heap since boot (systemHeapMinimum()). Unlike systemHeapStep() it logs every step. from_loop: straight to the debug log; else (the Matter task) through the
// log slots of the loop
void heap_step(const char* step, bool from_loop = false);

} // namespace matter
} // namespace espurna

void matterSetup();

bool matterStarted();

// For the other modules (the nspanel module: the pairing QR), through the head (modules_api.cpp): false / nothing
// without the matter module. The pairing window is open
bool matterWindowOpen();

// Opens the basic pairing window (MATTER.WINDOW); false while Matter is not started
bool matterWindowStart();

// QR payload ("MT:...") and manual pairing code; false while Matter is not started. They carry the setup passcode:
// for the owner's screens only (the MATTER panel, the NSPanel QR screen), never logged
bool matterPairingCodes(char* qr, size_t qr_size, char* manual, size_t manual_size);
