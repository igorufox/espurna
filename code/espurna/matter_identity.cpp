/*

MATTER: THE IDENTITY OF THE BOARD (one code on ESP32 and Beken)

- Factory data of a stock firmware (Quectel / Tuya tables of the Beken boards, the chip's factory_read()): its
  attestation (DAC, PAI, certification declaration and key), VID / PID / names.
- None: an own identity, made once per board and kept in the key-value store of the chip (namespace "espurna-id":
  ESP32 NVS, Beken FlashDB of the kvs partition, next to the fabrics of the stack): random setup passcode,
  discriminator and salt (+ the SPAKE2+ verifier), and a DAC with its own key - the SDK development DAC (VID 0xFFF1,
  PID 0x8000) with this board's serial number, public key and key identifier, signed by the public development PAI
  "Matter Dev PAI 0xFFF1 no PID" (connectedhomeip credentials/development). Controllers that accept the SDK example
  devices accept it; it is not a certified identity. An OTA update, a Matter factory reset leave it, so
  the QR code / pairing code stays.
- Custom: the owner's DAC, its private key, PAI and (optionally) certification declaration, uploaded in the MATTER panel
  of the WebUI and checked there (the key belongs to the DAC, the PAI signed the DAC), kept in the same namespace. VID /
  PID from the DAC subject; setup passcode and discriminator are the own ones above. The key is never sent back or
  logged. Without an uploaded declaration the one of the SDK examples is used (it covers VID 0xFFF1, PID 0x8000..0x8063)

`matterIdentity` (owner 11-16): "factory" / "espurna" (the own one) / "custom"; none = factory when the board has
factory data, else espurna, as before the setting. A choice the board cannot use falls back to espurna. setup() (the
loop, the module setup) reads it; when the identity differs from the one of the last start (the setting, another custom
DAC), the controllers are forgotten first (backend::reset(), as MATTER.RESET): they verified the old DAC.

The chip's part (arch/<chip>/matter_*.cpp): store_read() / store_write() of the key-value store, factory_read().
When: install() where the providers are set - the library's server init calls matter_identity_install() (below) on both
chips, in the Matter task; it loads the identity first (makes it on the first start).

*/

#include "espurna.h"

#if MATTER_SUPPORT

#include "espurna_matter.h"
#include "matter_identity.h"

#include <credentials/DeviceAttestationCredsProvider.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Base64.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/SetupPayload.h>

#include <cstring>
#include <memory>
#include <new>

namespace espurna {
namespace matter {
namespace identity {
namespace {

using namespace ::chip;
using namespace ::chip::DeviceLayer;

constexpr char KeyIdentity[] = "identity";
constexpr char KeyCertificate[] = "dac";

// The certificate parts and the SHA-1 below are the ones of the Beken library before (matter-bk7231n matter_app.cpp, now matter-espurna)

constexpr uint16_t VendorId  = 0xFFF1;
constexpr uint16_t ProductId = 0x8000;

// SDK development DAC FFF1/8000, the part to be signed. Serial number, public key and its key identifier are patched in
constexpr uint8_t DacTbs[] = {
    0x30, 0x82, 0x01, 0x8e, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x08, 0x23, 0x8a, 0x64, 0x7b, 0xbc, 0x4c, 0x30, 0xdd, 0x30,
    0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x3d, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55,
    0x04, 0x03, 0x0c, 0x1c, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x44, 0x65, 0x76, 0x20, 0x50, 0x41, 0x49, 0x20, 0x30,
    0x78, 0x46, 0x46, 0x46, 0x31, 0x20, 0x6e, 0x6f, 0x20, 0x50, 0x49, 0x44, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06,
    0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c, 0x04, 0x46, 0x46, 0x46, 0x31, 0x30, 0x20, 0x17, 0x0d, 0x32, 0x32,
    0x30, 0x32, 0x30, 0x35, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x18, 0x0f, 0x39, 0x39, 0x39, 0x39, 0x31, 0x32, 0x33,
    0x31, 0x32, 0x33, 0x35, 0x39, 0x35, 0x39, 0x5a, 0x30, 0x53, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c,
    0x1c, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x44, 0x65, 0x76, 0x20, 0x44, 0x41, 0x43, 0x20, 0x30, 0x78, 0x46, 0x46,
    0x46, 0x31, 0x2f, 0x30, 0x78, 0x38, 0x30, 0x30, 0x30, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01,
    0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c, 0x04, 0x46, 0x46, 0x46, 0x31, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01,
    0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x02, 0x0c, 0x04, 0x38, 0x30, 0x30, 0x30, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04,
    0x62, 0xdb, 0x16, 0xba, 0xde, 0xa3, 0x26, 0xa6, 0xdb, 0x84, 0x81, 0x4a, 0x06, 0x3f, 0xc6, 0xc7, 0xe9, 0xe2, 0xb1, 0x01,
    0xb7, 0x21, 0x64, 0x8e, 0xba, 0x4e, 0x5a, 0xc8, 0x40, 0xf5, 0xda, 0x30, 0x1e, 0xe6, 0x18, 0x12, 0x4e, 0xb4, 0x18, 0x0e,
    0x2f, 0xc3, 0xa2, 0x04, 0x7a, 0x56, 0x4b, 0xa9, 0xbc, 0xfa, 0x0b, 0xf7, 0x1f, 0x60, 0xce, 0x89, 0x30, 0xf1, 0xe7, 0xf6,
    0x6e, 0xc8, 0xd7, 0x28, 0xa3, 0x60, 0x30, 0x5e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x02,
    0x30, 0x00, 0x30, 0x0e, 0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x07, 0x80, 0x30, 0x1d,
    0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0xbc, 0xf7, 0xb0, 0x07, 0x49, 0x70, 0x63, 0x60, 0x6a, 0x26, 0xbe,
    0x4e, 0x08, 0x7c, 0x59, 0x56, 0x87, 0x74, 0x5a, 0x5a, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16,
    0x80, 0x14, 0x63, 0x54, 0x0e, 0x47, 0xf6, 0x4b, 0x1c, 0x38, 0xd1, 0x38, 0x84, 0xa4, 0x62, 0xd1, 0x6c, 0x19, 0x5d, 0x8f,
    0xfb, 0x3c,
};

constexpr size_t SerialOffset    = 11;
constexpr size_t SerialLength    = 8;
constexpr size_t PublicKeyOffset = 239;
constexpr size_t KeyIdOffset     = 349;

// ecdsa-with-SHA256
constexpr uint8_t SignatureAlgorithm[] = { 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };

// Matter-Development-PAI-FFF1-noPID key pair (public development credentials of connectedhomeip)
constexpr uint8_t PaiPublicKey[] = {
    0x04, 0x41, 0x9a, 0x93, 0x15, 0xc2, 0x17, 0x3e, 0x0c, 0x8c, 0x87, 0x6d, 0x03, 0xcc, 0xfc, 0x94, 0x48, 0x52, 0x64,
    0x7f, 0x7f, 0xec, 0x5e, 0x50, 0x82, 0xf4, 0x05, 0x99, 0x28, 0xec, 0xa8, 0x94, 0xc5, 0x94, 0x15, 0x13, 0x09, 0xac,
    0x63, 0x1e, 0x4c, 0xb0, 0x33, 0x92, 0xaf, 0x68, 0x4b, 0x0b, 0xaf, 0xb7, 0xe6, 0x5b, 0x3b, 0x81, 0x62, 0xc2, 0xf5,
    0x2b, 0xf9, 0x31, 0xb8, 0xe7, 0x7a, 0xaa, 0x82,
};

constexpr uint8_t PaiPrivateKey[] = {
    0xc1, 0xab, 0x5f, 0xe2, 0x84, 0xb4, 0xc1, 0x89, 0x40, 0xa5, 0x31, 0x61, 0xf8, 0x06, 0x94, 0x40,
    0x50, 0xa1, 0x69, 0x7b, 0x71, 0x76, 0x1d, 0x38, 0x53, 0x37, 0xa8, 0xa3, 0xcd, 0x09, 0x5c, 0x34,
};

// connectedhomeip credentials/examples: development PAI "Matter Dev PAI 0xFFF1 no PID" and the certification
// declaration of the SDK examples (VID 0xFFF1, PID 0x8000..0x8063)
constexpr uint8_t PaiCertificate[] = {
    0x30, 0x82, 0x01, 0xcb, 0x30, 0x82, 0x01, 0x71, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x08, 0x56, 0xad, 0x82, 0x22, 0xad,
    0x94, 0x5b, 0x64, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x30, 0x31, 0x18, 0x30,
    0x16, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0f, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20,
    0x50, 0x41, 0x41, 0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c,
    0x04, 0x46, 0x46, 0x46, 0x31, 0x30, 0x20, 0x17, 0x0d, 0x32, 0x32, 0x30, 0x32, 0x30, 0x35, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x5a, 0x18, 0x0f, 0x39, 0x39, 0x39, 0x39, 0x31, 0x32, 0x33, 0x31, 0x32, 0x33, 0x35, 0x39, 0x35, 0x39, 0x5a, 0x30,
    0x3d, 0x31, 0x25, 0x30, 0x23, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x1c, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x44,
    0x65, 0x76, 0x20, 0x50, 0x41, 0x49, 0x20, 0x30, 0x78, 0x46, 0x46, 0x46, 0x31, 0x20, 0x6e, 0x6f, 0x20, 0x50, 0x49, 0x44,
    0x31, 0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01, 0x0c, 0x04, 0x46, 0x46,
    0x46, 0x31, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x41, 0x9a, 0x93, 0x15, 0xc2, 0x17, 0x3e, 0x0c, 0x8c, 0x87, 0x6d,
    0x03, 0xcc, 0xfc, 0x94, 0x48, 0x52, 0x64, 0x7f, 0x7f, 0xec, 0x5e, 0x50, 0x82, 0xf4, 0x05, 0x99, 0x28, 0xec, 0xa8, 0x94,
    0xc5, 0x94, 0x15, 0x13, 0x09, 0xac, 0x63, 0x1e, 0x4c, 0xb0, 0x33, 0x92, 0xaf, 0x68, 0x4b, 0x0b, 0xaf, 0xb7, 0xe6, 0x5b,
    0x3b, 0x81, 0x62, 0xc2, 0xf5, 0x2b, 0xf9, 0x31, 0xb8, 0xe7, 0x7a, 0xaa, 0x82, 0xa3, 0x66, 0x30, 0x64, 0x30, 0x12, 0x06,
    0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x08, 0x30, 0x06, 0x01, 0x01, 0xff, 0x02, 0x01, 0x00, 0x30, 0x0e, 0x06,
    0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x01, 0x06, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e,
    0x04, 0x16, 0x04, 0x14, 0x63, 0x54, 0x0e, 0x47, 0xf6, 0x4b, 0x1c, 0x38, 0xd1, 0x38, 0x84, 0xa4, 0x62, 0xd1, 0x6c, 0x19,
    0x5d, 0x8f, 0xfb, 0x3c, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0x6a, 0xfd, 0x22,
    0x77, 0x1f, 0x51, 0x1f, 0xec, 0xbf, 0x16, 0x41, 0x97, 0x67, 0x10, 0xdc, 0xdc, 0x31, 0xa1, 0x71, 0x7e, 0x30, 0x0a, 0x06,
    0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45, 0x02, 0x21, 0x00, 0xb2, 0xef, 0x27,
    0xf4, 0x9a, 0xe9, 0xb5, 0x0f, 0xb9, 0x1e, 0xea, 0xc9, 0x4c, 0x4d, 0x0b, 0xdb, 0xb8, 0xd7, 0x92, 0x9c, 0x6c, 0xb8, 0x8f,
    0xac, 0xe5, 0x29, 0x36, 0x8d, 0x12, 0x05, 0x4c, 0x0c, 0x02, 0x20, 0x65, 0x5d, 0xc9, 0x2b, 0x86, 0xbd, 0x90, 0x98, 0x82,
    0xa6, 0xc6, 0x21, 0x77, 0xb8, 0x25, 0xd7, 0xd0, 0x5e, 0xdb, 0xe7, 0xc2, 0x2f, 0x9f, 0xea, 0x71, 0x22, 0x0e, 0x7e, 0xa7,
    0x03, 0xf8, 0x91,
};

constexpr uint8_t CertificationDeclaration[] = {
    0x30, 0x82, 0x02, 0x17, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02, 0xa0, 0x82, 0x02, 0x08, 0x30,
    0x82, 0x02, 0x04, 0x02, 0x01, 0x03, 0x31, 0x0d, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
    0x01, 0x30, 0x82, 0x01, 0x70, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01, 0xa0, 0x82, 0x01, 0x61,
    0x04, 0x82, 0x01, 0x5d, 0x15, 0x24, 0x00, 0x01, 0x25, 0x01, 0xf1, 0xff, 0x36, 0x02, 0x05, 0x00, 0x80, 0x05, 0x01, 0x80,
    0x05, 0x02, 0x80, 0x05, 0x03, 0x80, 0x05, 0x04, 0x80, 0x05, 0x05, 0x80, 0x05, 0x06, 0x80, 0x05, 0x07, 0x80, 0x05, 0x08,
    0x80, 0x05, 0x09, 0x80, 0x05, 0x0a, 0x80, 0x05, 0x0b, 0x80, 0x05, 0x0c, 0x80, 0x05, 0x0d, 0x80, 0x05, 0x0e, 0x80, 0x05,
    0x0f, 0x80, 0x05, 0x10, 0x80, 0x05, 0x11, 0x80, 0x05, 0x12, 0x80, 0x05, 0x13, 0x80, 0x05, 0x14, 0x80, 0x05, 0x15, 0x80,
    0x05, 0x16, 0x80, 0x05, 0x17, 0x80, 0x05, 0x18, 0x80, 0x05, 0x19, 0x80, 0x05, 0x1a, 0x80, 0x05, 0x1b, 0x80, 0x05, 0x1c,
    0x80, 0x05, 0x1d, 0x80, 0x05, 0x1e, 0x80, 0x05, 0x1f, 0x80, 0x05, 0x20, 0x80, 0x05, 0x21, 0x80, 0x05, 0x22, 0x80, 0x05,
    0x23, 0x80, 0x05, 0x24, 0x80, 0x05, 0x25, 0x80, 0x05, 0x26, 0x80, 0x05, 0x27, 0x80, 0x05, 0x28, 0x80, 0x05, 0x29, 0x80,
    0x05, 0x2a, 0x80, 0x05, 0x2b, 0x80, 0x05, 0x2c, 0x80, 0x05, 0x2d, 0x80, 0x05, 0x2e, 0x80, 0x05, 0x2f, 0x80, 0x05, 0x30,
    0x80, 0x05, 0x31, 0x80, 0x05, 0x32, 0x80, 0x05, 0x33, 0x80, 0x05, 0x34, 0x80, 0x05, 0x35, 0x80, 0x05, 0x36, 0x80, 0x05,
    0x37, 0x80, 0x05, 0x38, 0x80, 0x05, 0x39, 0x80, 0x05, 0x3a, 0x80, 0x05, 0x3b, 0x80, 0x05, 0x3c, 0x80, 0x05, 0x3d, 0x80,
    0x05, 0x3e, 0x80, 0x05, 0x3f, 0x80, 0x05, 0x40, 0x80, 0x05, 0x41, 0x80, 0x05, 0x42, 0x80, 0x05, 0x43, 0x80, 0x05, 0x44,
    0x80, 0x05, 0x45, 0x80, 0x05, 0x46, 0x80, 0x05, 0x47, 0x80, 0x05, 0x48, 0x80, 0x05, 0x49, 0x80, 0x05, 0x4a, 0x80, 0x05,
    0x4b, 0x80, 0x05, 0x4c, 0x80, 0x05, 0x4d, 0x80, 0x05, 0x4e, 0x80, 0x05, 0x4f, 0x80, 0x05, 0x50, 0x80, 0x05, 0x51, 0x80,
    0x05, 0x52, 0x80, 0x05, 0x53, 0x80, 0x05, 0x54, 0x80, 0x05, 0x55, 0x80, 0x05, 0x56, 0x80, 0x05, 0x57, 0x80, 0x05, 0x58,
    0x80, 0x05, 0x59, 0x80, 0x05, 0x5a, 0x80, 0x05, 0x5b, 0x80, 0x05, 0x5c, 0x80, 0x05, 0x5d, 0x80, 0x05, 0x5e, 0x80, 0x05,
    0x5f, 0x80, 0x05, 0x60, 0x80, 0x05, 0x61, 0x80, 0x05, 0x62, 0x80, 0x05, 0x63, 0x80, 0x18, 0x24, 0x03, 0x16, 0x2c, 0x04,
    0x13, 0x43, 0x53, 0x41, 0x30, 0x30, 0x30, 0x30, 0x30, 0x53, 0x57, 0x43, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2d, 0x30, 0x30,
    0x24, 0x05, 0x00, 0x24, 0x06, 0x00, 0x24, 0x07, 0x01, 0x24, 0x08, 0x00, 0x18, 0x31, 0x7c, 0x30, 0x7a, 0x02, 0x01, 0x03,
    0x80, 0x14, 0xfe, 0x34, 0x3f, 0x95, 0x99, 0x47, 0x76, 0x3b, 0x61, 0xee, 0x45, 0x39, 0x13, 0x13, 0x38, 0x49, 0x4f, 0xe6,
    0x7d, 0x8e, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x30, 0x0a, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x04, 0x46, 0x30, 0x44, 0x02, 0x20, 0x4a, 0x12, 0xf8, 0xd4, 0x2f, 0x90, 0x23,
    0x5c, 0x05, 0xa7, 0x71, 0x21, 0xcb, 0xeb, 0xae, 0x15, 0xd5, 0x90, 0x14, 0x65, 0x58, 0xe9, 0xc9, 0xb4, 0x7a, 0x1a, 0x38,
    0xf7, 0xa3, 0x6a, 0x7d, 0xc5, 0x02, 0x20, 0x20, 0xa4, 0x74, 0x28, 0x97, 0xc3, 0x0a, 0xed, 0xa0, 0xa5, 0x6b, 0x36, 0xe1,
    0x4e, 0xbb, 0xc8, 0x5b, 0xbd, 0xb7, 0x44, 0x93, 0xf9, 0x93, 0x58, 0x1e, 0xb0, 0x44, 0x4e, 0xd6, 0xca, 0x94, 0x0b,
};

// SHA-1 of the public key, the key identifier of the certificate (mbedTLS of the library is built without SHA-1)
void Sha1(const uint8_t * data, size_t length, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t block[64];
    const uint64_t bits = static_cast<uint64_t>(length) * 8;
    size_t total = ((length + 8) / 64 + 1) * 64;

    for (size_t offset = 0; offset < total; offset += 64)
    {
        for (size_t i = 0; i < 64; ++i)
        {
            const size_t index = offset + i;
            if (index < length)
                block[i] = data[index];
            else if (index == length)
                block[i] = 0x80;
            else if (index >= total - 8)
                block[i] = static_cast<uint8_t>(bits >> (8 * (total - 1 - index)));
            else
                block[i] = 0;
        }

        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) |
                uint32_t(block[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
        {
            const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i]             = (v << 1) | (v >> 31);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)
            {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e                = d;
            d                = c;
            c                = (b << 30) | (b >> 2);
            b                = a;
            a                = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    for (int i = 0; i < 5; ++i)
    {
        out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

constexpr uint32_t Magic { 0x45494d31 }; // "EMI1"
constexpr uint32_t Iterations { 1000 };
constexpr size_t SaltLength { 32 };
constexpr size_t KeypairLength { Crypto::kP256_PublicKey_Length + Crypto::kP256_PrivateKey_Length };

struct Stored {
    uint32_t magic;
    uint32_t passcode;
    uint16_t discriminator;
    uint16_t reserved;
    uint32_t iterations;
    uint8_t salt[SaltLength];
    uint8_t verifier[Crypto::kSpake2p_VerifierSerialized_Length];
    uint8_t serial[SerialLength];
    uint8_t keypair[KeypairLength];
};

Stored stored;

// The custom identity: the uploaded DAC, PAI and declaration are kept as they came ("c-dac", "c-pai", "c-cd"), this
// header ("custom") says how long they are and holds the key pair (the DAC's public key + the private key)
constexpr char KeyCustom[] = "custom";
constexpr char KeyCustomDac[] = "c-dac";
constexpr char KeyCustomPai[] = "c-pai";
constexpr char KeyCustomDeclaration[] = "c-cd";

constexpr uint32_t CustomMagic { 0x45494d43 }; // "EIMC"
constexpr size_t CertificateMax { Crypto::kMax_x509_Certificate_Length };
constexpr size_t DeclarationMax { 800 };
constexpr size_t PrivateKeyMax { 200 };
constexpr size_t Sha1Length { 20 };

struct Custom {
    uint32_t magic;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t dac_length;
    uint16_t pai_length;
    uint16_t declaration_length;
    uint16_t reserved;
    uint8_t dac_sha1[Sha1Length];
    uint8_t keypair[KeypairLength];
};

Custom custom;

// Which identity the last start used ("used"): another one now = the controllers are forgotten (setup())
constexpr char KeyUsed[] = "used";
constexpr uint32_t UsedMagic { 0x45494d55 }; // "EIMU"

struct Used {
    uint32_t magic;
    uint8_t source;
    uint8_t reserved[3];
    uint8_t dac_sha1[Sha1Length];
};

enum class Source : uint8_t {
    None,
    Factory,
    Own,
    Custom,
};

// in use, set in the Matter task by load()
Source source { Source::None };
// what setup() chose from `matterIdentity` and what the board has, in the loop before the start
Source chosen { Source::None };
// `matterIdentity` asked for one the board cannot use (no factory data, nothing uploaded)
bool fallback { false };
bool factory_present { false };
bool custom_present { false };
bool installed { false };

// -----------------------------------------------------------------------------
// Factory data

CHIP_ERROR read_factory(const char* name, MutableByteSpan& out) {
    size_t length { 0 };
    if (!factory_read(name, out.data(), out.size(), length)) {
        return CHIP_ERROR_NOT_FOUND;
    }

    if (length > out.size()) {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }

    out.reduce_size(length);
    return CHIP_NO_ERROR;
}

bool has_factory() {
    size_t length { 0 };
    return factory_read("dac-cert", nullptr, 0, length) && length;
}

// dac-cert, pai-cert, cert-dclrn, dac-pub-key and dac-key as the stock firmware stored them
class FactoryAttestation final : public Credentials::DeviceAttestationCredentialsProvider {
public:
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan& out) override {
        return read_factory("cert-dclrn", out);
    }

    CHIP_ERROR GetFirmwareInformation(MutableByteSpan& out) override {
        out.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan& out) override {
        return read_factory("dac-cert", out);
    }

    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan& out) override {
        return read_factory("pai-cert", out);
    }

    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan& message, MutableByteSpan& out) override {
        Crypto::P256SerializedKeypair serialized;
        MutableByteSpan pub(serialized.Bytes(), Crypto::kP256_PublicKey_Length);
        MutableByteSpan priv(serialized.Bytes() + Crypto::kP256_PublicKey_Length, Crypto::kP256_PrivateKey_Length);

        auto err = read_factory("dac-pub-key", pub);
        if (err == CHIP_NO_ERROR) {
            err = read_factory("dac-key", priv);
        }
        if ((err == CHIP_NO_ERROR)
            && ((pub.size() != Crypto::kP256_PublicKey_Length) || (priv.size() != Crypto::kP256_PrivateKey_Length)))
        {
            err = CHIP_ERROR_INVALID_ARGUMENT;
        }

        Crypto::P256ECDSASignature signature;
        if (err == CHIP_NO_ERROR) {
            serialized.SetLength(KeypairLength);
            Crypto::P256Keypair key;
            err = key.Deserialize(serialized);
            if (err == CHIP_NO_ERROR) {
                err = key.ECDSA_sign_msg(message.data(), message.size(), signature);
            }
        }

        std::memset(serialized.Bytes(), 0, serialized.Capacity());
        ReturnErrorOnFailure(err);

        return CopySpanToMutableSpan(ByteSpan(signature.ConstBytes(), signature.Length()), out);
    }
};

// The own and the custom identity: not the factory data
bool board_made() {
    return (source == Source::Own) || (source == Source::Custom);
}

// VID / PID / names: the factory data (the certification declaration is issued for them), the own development ones or
// the custom DAC's; the rest from the provider of the platform
class InstanceInfo final : public DeviceInstanceInfoProvider {
public:
    void base(DeviceInstanceInfoProvider* base) {
        _base = base;
    }

    CHIP_ERROR GetVendorName(char* buf, size_t size) override {
        if (board_made()) {
            return copy("ESPurna", buf, size);
        }
        return (text("vendor-name", buf, size) == CHIP_NO_ERROR) ? CHIP_NO_ERROR : _base->GetVendorName(buf, size);
    }

    CHIP_ERROR GetVendorId(uint16_t& out) override {
        if (board_made()) {
            out = (source == Source::Custom) ? custom.vendor_id : VendorId;
            return CHIP_NO_ERROR;
        }
        return number("vendor-id", out) ? CHIP_NO_ERROR : _base->GetVendorId(out);
    }

    CHIP_ERROR GetProductName(char* buf, size_t size) override {
        if (board_made()) {
            return copy("ESPurna plug", buf, size);
        }
        return (text("product-name", buf, size) == CHIP_NO_ERROR) ? CHIP_NO_ERROR : _base->GetProductName(buf, size);
    }

    CHIP_ERROR GetProductId(uint16_t& out) override {
        if (board_made()) {
            out = (source == Source::Custom) ? custom.product_id : ProductId;
            return CHIP_NO_ERROR;
        }
        return number("product-id", out) ? CHIP_NO_ERROR : _base->GetProductId(out);
    }

    CHIP_ERROR GetPartNumber(char* buf, size_t size) override {
        return _base->GetPartNumber(buf, size);
    }

    CHIP_ERROR GetProductURL(char* buf, size_t size) override {
        return _base->GetProductURL(buf, size);
    }

    CHIP_ERROR GetProductLabel(char* buf, size_t size) override {
        return _base->GetProductLabel(buf, size);
    }

    // the board's (matter.cpp, its chip id) with the own and the custom identity; the factory one may have its own in
    // the factory data
    CHIP_ERROR GetSerialNumber(char* buf, size_t size) override {
        const char* serial = matter_serial_number();
        if (board_made() && serial && *serial) {
            return copy(serial, buf, size);
        }
        return _base->GetSerialNumber(buf, size);
    }

    CHIP_ERROR GetManufacturingDate(uint16_t& year, uint8_t& month, uint8_t& day) override {
        return _base->GetManufacturingDate(year, month, day);
    }

    CHIP_ERROR GetHardwareVersion(uint16_t& out) override {
        return _base->GetHardwareVersion(out);
    }

    CHIP_ERROR GetHardwareVersionString(char* buf, size_t size) override {
        return _base->GetHardwareVersionString(buf, size);
    }

    CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan& out) override {
        return _base->GetRotatingDeviceIdUniqueId(out);
    }

private:
    static CHIP_ERROR copy(const char* value, char* buf, size_t size) {
        const size_t length = std::strlen(value);
        VerifyOrReturnError(length < size, CHIP_ERROR_BUFFER_TOO_SMALL);
        std::memcpy(buf, value, length + 1);
        return CHIP_NO_ERROR;
    }

    static CHIP_ERROR text(const char* name, char* buf, size_t size) {
        VerifyOrReturnError(size > 1, CHIP_ERROR_BUFFER_TOO_SMALL);
        MutableByteSpan span(reinterpret_cast<uint8_t*>(buf), size - 1);
        ReturnErrorOnFailure(read_factory(name, span));
        VerifyOrReturnError(span.size() > 0, CHIP_ERROR_NOT_FOUND);
        buf[span.size()] = '\0';
        return CHIP_NO_ERROR;
    }

    // 2 bytes, little endian
    static bool number(const char* name, uint16_t& out) {
        uint8_t value[2];
        MutableByteSpan span(value);
        if ((read_factory(name, span) != CHIP_NO_ERROR) || (span.size() != sizeof(value))) {
            return false;
        }

        out = static_cast<uint16_t>(value[0] | (value[1] << 8));
        return true;
    }

    DeviceInstanceInfoProvider* _base { nullptr };
};

// -----------------------------------------------------------------------------
// Own identity

bool read_own() {
    size_t length { 0 };
    return store_read(KeyIdentity, reinterpret_cast<uint8_t*>(&stored), sizeof(stored), length)
        && (length == sizeof(stored)) && (stored.magic == Magic);
}

// DAC: the development template with this board's serial, key and key identifier, signed with the development PAI key
CHIP_ERROR make_certificate(Crypto::P256Keypair& key) {
    // static: the stack of the Matter task (Beken 6 KiB) is mostly needed by the crypto below
    static uint8_t tbs[sizeof(DacTbs)];
    std::memcpy(tbs, DacTbs, sizeof(tbs));
    std::memcpy(&tbs[SerialOffset], stored.serial, SerialLength);

    const auto& pub = key.Pubkey();
    std::memcpy(&tbs[PublicKeyOffset], pub.ConstBytes(), Crypto::kP256_PublicKey_Length);

    uint8_t key_id[20];
    Sha1(pub.ConstBytes(), Crypto::kP256_PublicKey_Length, key_id);
    std::memcpy(&tbs[KeyIdOffset], key_id, sizeof(key_id));

    Crypto::P256Keypair pai;
    Crypto::P256SerializedKeypair pai_serialized;
    std::memcpy(pai_serialized.Bytes(), PaiPublicKey, sizeof(PaiPublicKey));
    std::memcpy(pai_serialized.Bytes() + sizeof(PaiPublicKey), PaiPrivateKey, sizeof(PaiPrivateKey));
    pai_serialized.SetLength(sizeof(PaiPublicKey) + sizeof(PaiPrivateKey));
    ReturnErrorOnFailure(pai.Deserialize(pai_serialized));

    Crypto::P256ECDSASignature raw;
    ReturnErrorOnFailure(pai.ECDSA_sign_msg(tbs, sizeof(tbs), raw));

    static uint8_t signature[Crypto::kMax_ECDSA_Signature_Length_Der];
    MutableByteSpan signature_span(signature);
    ReturnErrorOnFailure(Crypto::EcdsaRawSignatureToAsn1(Crypto::kP256_FE_Length,
        ByteSpan(raw.ConstBytes(), raw.Length()), signature_span));

    // SEQUENCE { tbs, signature algorithm, BIT STRING { 0 unused bits, signature } }
    const size_t bits = 1 + signature_span.size();
    const size_t body = sizeof(tbs) + sizeof(SignatureAlgorithm) + 2 + bits;

    static uint8_t certificate[4 + sizeof(DacTbs) + sizeof(SignatureAlgorithm) + 3 + sizeof(signature)];
    size_t length { 0 };
    certificate[length++] = 0x30;
    certificate[length++] = 0x82;
    certificate[length++] = static_cast<uint8_t>((body >> 8) & 0xff);
    certificate[length++] = static_cast<uint8_t>(body & 0xff);
    std::memcpy(&certificate[length], tbs, sizeof(tbs));
    length += sizeof(tbs);
    std::memcpy(&certificate[length], SignatureAlgorithm, sizeof(SignatureAlgorithm));
    length += sizeof(SignatureAlgorithm);
    certificate[length++] = 0x03;
    certificate[length++] = static_cast<uint8_t>(bits);
    certificate[length++] = 0x00;
    std::memcpy(&certificate[length], signature_span.data(), signature_span.size());
    length += signature_span.size();

    if (!store_write(KeyCertificate, certificate, length)) {
        return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
    }

    return CHIP_NO_ERROR;
}

// Once per board: codes, salt, verifier, key and certificate
CHIP_ERROR make_own() {
    static Stored made;
    made = Stored{};
    made.magic = Magic;

    do {
        ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t*>(&made.passcode), sizeof(made.passcode)));
        made.passcode = (made.passcode % 99999998u) + 1u;
    } while (!SetupPayload::IsValidSetupPIN(made.passcode));

    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t*>(&made.discriminator),
        sizeof(made.discriminator)));
    made.discriminator = static_cast<uint16_t>(made.discriminator & 0xfff);

    made.iterations = Iterations;
    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(made.salt, sizeof(made.salt)));

    Crypto::Spake2pVerifier verifier;
    ReturnErrorOnFailure(verifier.Generate(made.iterations, ByteSpan(made.salt), made.passcode));
    MutableByteSpan verifier_span(made.verifier);
    ReturnErrorOnFailure(verifier.Serialize(verifier_span));

    ReturnErrorOnFailure(Crypto::DRBG_get_bytes(made.serial, sizeof(made.serial)));
    // positive, and minimal as a DER integer
    made.serial[0] = static_cast<uint8_t>((made.serial[0] & 0x3f) | 0x40);

    Crypto::P256Keypair key;
    ReturnErrorOnFailure(key.Initialize(Crypto::ECPKeyTarget::ECDSA));
    Crypto::P256SerializedKeypair serialized;
    ReturnErrorOnFailure(key.Serialize(serialized));
    VerifyOrReturnError(serialized.Length() == sizeof(made.keypair), CHIP_ERROR_INTERNAL);
    std::memcpy(made.keypair, serialized.ConstBytes(), sizeof(made.keypair));
    std::memset(serialized.Bytes(), 0, serialized.Capacity());

    stored = made;
    std::memset(&made, 0, sizeof(made));
    ReturnErrorOnFailure(make_certificate(key));

    if (!store_write(KeyIdentity, reinterpret_cast<const uint8_t*>(&stored), sizeof(stored))) {
        return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
    }

    return CHIP_NO_ERROR;
}

// Key, two signatures and PBKDF2 take seconds on the BK7231N. A task above the loop (the Matter task of Beken) would
// keep the loop, which feeds the hardware watchdog, from running all that time: below it until done
CHIP_ERROR make() {
    const auto priority = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, tskIDLE_PRIORITY + 1);
    const auto err = make_own();
    vTaskPrioritySet(nullptr, priority);
    return err;
}

class OwnCommissionableData final : public CommissionableDataProvider {
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t& out) override {
        out = stored.discriminator;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetSetupDiscriminator(uint16_t) override {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t& out) override {
        out = stored.iterations;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(MutableByteSpan& out) override {
        return CopySpanToMutableSpan(ByteSpan(stored.salt), out);
    }

    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan& out, size_t& length) override {
        length = sizeof(stored.verifier);
        return CopySpanToMutableSpan(ByteSpan(stored.verifier), out);
    }

    CHIP_ERROR GetSetupPasscode(uint32_t& out) override {
        out = stored.passcode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetSetupPasscode(uint32_t) override {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
};

// A certificate (or the declaration) of the key-value store
CHIP_ERROR read_store(const char* name, MutableByteSpan& out) {
    size_t length { 0 };
    if (!store_read(name, out.data(), out.size(), length)) {
        return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
    }

    VerifyOrReturnError(length <= out.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
    out.reduce_size(length);
    return CHIP_NO_ERROR;
}

// The DAC's public key + its private key, as P256Keypair::Serialize() gives them
CHIP_ERROR sign_with(const uint8_t (&keypair)[KeypairLength], const ByteSpan& message, MutableByteSpan& out) {
    Crypto::P256SerializedKeypair serialized;
    std::memcpy(serialized.Bytes(), keypair, KeypairLength);
    serialized.SetLength(KeypairLength);

    Crypto::P256Keypair key;
    const auto err = key.Deserialize(serialized);
    std::memset(serialized.Bytes(), 0, serialized.Capacity());
    ReturnErrorOnFailure(err);

    Crypto::P256ECDSASignature signature;
    ReturnErrorOnFailure(key.ECDSA_sign_msg(message.data(), message.size(), signature));
    return CopySpanToMutableSpan(ByteSpan(signature.ConstBytes(), signature.Length()), out);
}

// Own DAC and key; the development PAI and the certification declaration of the SDK examples
class OwnAttestation final : public Credentials::DeviceAttestationCredentialsProvider {
public:
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan& out) override {
        return CopySpanToMutableSpan(ByteSpan(CertificationDeclaration), out);
    }

    CHIP_ERROR GetFirmwareInformation(MutableByteSpan& out) override {
        out.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan& out) override {
        return CopySpanToMutableSpan(ByteSpan(PaiCertificate), out);
    }

    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan& out) override {
        return read_store(KeyCertificate, out);
    }

    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan& message, MutableByteSpan& out) override {
        return sign_with(stored.keypair, message, out);
    }
};

// The uploaded DAC, PAI and key; the uploaded declaration, else the one of the SDK examples
class CustomAttestation final : public Credentials::DeviceAttestationCredentialsProvider {
public:
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan& out) override {
        if (!custom.declaration_length) {
            return CopySpanToMutableSpan(ByteSpan(CertificationDeclaration), out);
        }
        return read_store(KeyCustomDeclaration, out);
    }

    CHIP_ERROR GetFirmwareInformation(MutableByteSpan& out) override {
        out.reduce_size(0);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan& out) override {
        return read_store(KeyCustomPai, out);
    }

    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan& out) override {
        return read_store(KeyCustomDac, out);
    }

    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan& message, MutableByteSpan& out) override {
        return sign_with(custom.keypair, message, out);
    }
};

FactoryAttestation factory_attestation;
InstanceInfo instance_info;
OwnCommissionableData own_commissionable;
OwnAttestation own_attestation;
CustomAttestation custom_attestation;

// -----------------------------------------------------------------------------
// Custom identity: reading, checking and storing it

void wipe_custom() {
    std::memset(&custom, 0, sizeof(custom));
}

bool read_custom() {
    size_t length { 0 };
    const auto out = store_read(KeyCustom, reinterpret_cast<uint8_t*>(&custom), sizeof(custom), length)
        && (length == sizeof(custom)) && (custom.magic == CustomMagic)
        && custom.dac_length && (custom.dac_length <= CertificateMax)
        && custom.pai_length && (custom.pai_length <= CertificateMax)
        && (custom.declaration_length <= DeclarationMax);
    if (!out) {
        wipe_custom();
    }

    return out;
}

// One DER element at the start of `in`: its tag, content and full length (header + content)
struct Der {
    uint8_t tag { 0 };
    const uint8_t* content { nullptr };
    size_t length { 0 };
    size_t total { 0 };
};

bool der(const uint8_t* in, size_t size, Der& out) {
    if (!in || (size < 2)) {
        return false;
    }

    size_t header { 2 };
    size_t length { in[1] };
    if (length & 0x80) {
        const size_t bytes = length & 0x7f;
        if (!bytes || (bytes > 2) || (size < (2 + bytes))) {
            return false;
        }

        length = 0;
        for (size_t index = 0; index < bytes; ++index) {
            length = (length << 8) | in[2 + index];
        }
        header += bytes;
    }

    if (length > (size - header)) {
        return false;
    }

    out.tag = in[0];
    out.content = in + header;
    out.length = length;
    out.total = header + length;
    return true;
}

// The private key as the factory tables keep it ("dac-key": the 32 bytes of the scalar), or DER: SEC1 ECPrivateKey
// (openssl ec), PKCS#8 PrivateKeyInfo around it (openssl genpkey, chip-cert). PEM is made DER by the WebUI
bool private_key(const uint8_t* in, size_t size, uint8_t (&out)[Crypto::kP256_PrivateKey_Length], int depth = 0) {
    if (size == Crypto::kP256_PrivateKey_Length) {
        std::memcpy(out, in, size);
        return true;
    }

    Der sequence;
    if ((depth > 1) || !der(in, size, sequence) || (sequence.tag != 0x30)) {
        return false;
    }

    const uint8_t* next = sequence.content;
    size_t left = sequence.length;

    Der version;
    if (!der(next, left, version) || (version.tag != 0x02) || (version.length != 1)) {
        return false;
    }
    next += version.total;
    left -= version.total;

    // SEC1: version 1, OCTET STRING of the scalar
    if (version.content[0] == 1) {
        Der key;
        if (!der(next, left, key) || (key.tag != 0x04) || !key.length || (key.length > sizeof(out))) {
            return false;
        }

        std::memset(out, 0, sizeof(out));
        std::memcpy(&out[sizeof(out) - key.length], key.content, key.length);
        return true;
    }

    // PKCS#8: version 0, the algorithm, OCTET STRING of the SEC1 key
    if (version.content[0] == 0) {
        Der algorithm;
        if (!der(next, left, algorithm) || (algorithm.tag != 0x30)) {
            return false;
        }
        next += algorithm.total;
        left -= algorithm.total;

        Der octets;
        if (!der(next, left, octets) || (octets.tag != 0x04)) {
            return false;
        }

        return private_key(octets.content, octets.length, out, depth + 1);
    }

    return false;
}

// The PAI's key signed the DAC: SEQUENCE { tbs, ecdsa-with-SHA256, BIT STRING { 0, signature } }
CHIP_ERROR verify_issued(const ByteSpan& dac, const ByteSpan& pai) {
    Crypto::P256PublicKey issuer;
    ReturnErrorOnFailure(Crypto::ExtractPubkeyFromX509Cert(pai, issuer));

    Der certificate;
    VerifyOrReturnError(der(dac.data(), dac.size(), certificate) && (certificate.tag == 0x30),
        CHIP_ERROR_INVALID_ARGUMENT);

    const uint8_t* next = certificate.content;
    size_t left = certificate.length;

    Der tbs;
    VerifyOrReturnError(der(next, left, tbs) && (tbs.tag == 0x30), CHIP_ERROR_INVALID_ARGUMENT);
    const uint8_t* tbs_data = next;
    const size_t tbs_length = tbs.total;
    next += tbs.total;
    left -= tbs.total;

    Der algorithm;
    VerifyOrReturnError(der(next, left, algorithm) && (algorithm.total == sizeof(SignatureAlgorithm))
        && (std::memcmp(next, SignatureAlgorithm, sizeof(SignatureAlgorithm)) == 0), CHIP_ERROR_INVALID_ARGUMENT);
    next += algorithm.total;
    left -= algorithm.total;

    Der bits;
    VerifyOrReturnError(der(next, left, bits) && (bits.tag == 0x03) && (bits.length > 1) && (bits.content[0] == 0),
        CHIP_ERROR_INVALID_ARGUMENT);

    Crypto::P256ECDSASignature signature;
    MutableByteSpan raw(signature.Bytes(), signature.Capacity());
    ReturnErrorOnFailure(Crypto::EcdsaAsn1SignatureToRaw(Crypto::kP256_FE_Length,
        ByteSpan(bits.content + 1, bits.length - 1), raw));
    ReturnErrorOnFailure(signature.SetLength(raw.size()));

    return issuer.ECDSA_validate_msg_signature(tbs_data, tbs_length, signature);
}

// Base64 of the WebUI; empty = none. false: not base64 or longer than `max`
bool decode(const char* in, size_t max, std::unique_ptr<uint8_t[]>& out, size_t& length) {
    length = 0;
    const size_t size = in ? std::strlen(in) : 0;
    if (!size) {
        return true;
    }

    if (size > (((max + 2) / 3) * 4)) {
        return false;
    }

    out.reset(new (std::nothrow) uint8_t[((size + 3) / 4) * 3]);
    if (!out) {
        return false;
    }

    const auto decoded = Base64Decode32(in, static_cast<uint32_t>(size), out.get());
    if ((decoded == UINT32_MAX) || (decoded > max)) {
        return false;
    }

    length = decoded;
    return true;
}

Choice choice_of(Source value) {
    switch (value) {
    case Source::Factory:
        return Choice::Factory;
    case Source::Custom:
        return Choice::Custom;
    case Source::None:
    case Source::Own:
        break;
    }

    return Choice::Espurna;
}

} // namespace

const char* name(Choice value) {
    switch (value) {
    case Choice::Factory:
        return "factory";
    case Choice::Espurna:
        return "espurna";
    case Choice::Custom:
        return "custom";
    }

    return "espurna";
}

bool parse(const char* value, Choice& out) {
    if (!value) {
        return false;
    }

    for (const auto choice : {Choice::Factory, Choice::Espurna, Choice::Custom}) {
        if (std::strcmp(value, name(choice)) == 0) {
            out = choice;
            return true;
        }
    }

    return false;
}

void setup() {
    factory_present = has_factory();
    custom_present = read_custom();

    Source out = factory_present ? Source::Factory : Source::Own;

    Choice choice;
    const auto setting = getSetting(Setting);
    if (parse(setting.c_str(), choice)) {
        switch (choice) {
        case Choice::Factory:
            out = factory_present ? Source::Factory : Source::Own;
            break;
        case Choice::Espurna:
            out = Source::Own;
            break;
        case Choice::Custom:
            out = custom_present ? Source::Custom : Source::Own;
            break;
        }

        fallback = (choice_of(out) != choice);
        if (fallback) {
            DEBUG_MSG_P(PSTR("[MATTER] %s identity not available on this board, own identity used\n"), name(choice));
        }
    }

    chosen = out;
    if (out != Source::Custom) {
        // the VID / PID stay for the panel, the key is not needed
        std::memset(custom.keypair, 0, sizeof(custom.keypair));
    }

    Used used {};
    used.magic = UsedMagic;
    used.source = static_cast<uint8_t>(out);
    if (out == Source::Custom) {
        std::memcpy(used.dac_sha1, custom.dac_sha1, sizeof(used.dac_sha1));
    }

    Used last {};
    size_t length { 0 };
    const auto known = store_read(KeyUsed, reinterpret_cast<uint8_t*>(&last), sizeof(last), length)
        && (length == sizeof(last)) && (last.magic == UsedMagic);
    if (known && (std::memcmp(&last, &used, sizeof(used)) == 0)) {
        return;
    }

    // the first start with this record keeps the controllers: the identity is the one of the automatic choice before
    if (known) {
        DEBUG_MSG_P(PSTR("[MATTER] Identity changed (%s), forgetting the controllers: they verified the old DAC\n"),
            name(choice_of(out)));
        if (!backend::reset()) {
            DEBUG_MSG_P(PSTR("[MATTER] Matter storage failed, the controllers are kept\n"));
            return;
        }
    }

    store_write(KeyUsed, reinterpret_cast<const uint8_t*>(&used), sizeof(used));
}

Choice choice() {
    return choice_of((source != Source::None) ? source : chosen);
}

bool available(Choice value) {
    switch (value) {
    case Choice::Factory:
        return factory_present;
    case Choice::Espurna:
        return true;
    case Choice::Custom:
        return custom_present;
    }

    return false;
}

bool custom_ids(uint16_t& vendor, uint16_t& product) {
    if (!custom_present) {
        return false;
    }

    vendor = custom.vendor_id;
    product = custom.product_id;
    return true;
}

bool ids(uint16_t& vendor, uint16_t& product) {
    switch (choice()) {
    case Choice::Factory:
        return false;
    case Choice::Espurna:
        vendor = VendorId;
        product = ProductId;
        return true;
    case Choice::Custom:
        return custom_ids(vendor, product);
    }

    return false;
}

bool custom_store(const char* dac_text, const char* key_text, const char* pai_text, const char* declaration_text,
    espurna::string& error)
{
    std::unique_ptr<uint8_t[]> dac;
    std::unique_ptr<uint8_t[]> key;
    std::unique_ptr<uint8_t[]> pai;
    std::unique_ptr<uint8_t[]> declaration;
    size_t dac_length { 0 };
    size_t key_length { 0 };
    size_t pai_length { 0 };
    size_t declaration_length { 0 };

    if (!decode(dac_text, CertificateMax, dac, dac_length) || !dac_length) {
        error = "DAC: missing, not base64 or longer than 600 bytes";
        return false;
    }

    if (!decode(pai_text, CertificateMax, pai, pai_length) || !pai_length) {
        error = "PAI: missing, not base64 or longer than 600 bytes";
        return false;
    }

    if (!decode(declaration_text, DeclarationMax, declaration, declaration_length)) {
        error = "Certification declaration: not base64 or longer than 800 bytes";
        return false;
    }

    // the key buffer (all of it, decode() allocates by the base64 length) is wiped on every way out
    struct Wipe {
        std::unique_ptr<uint8_t[]>& data;
        size_t capacity;
        ~Wipe() {
            if (data) {
                std::memset(data.get(), 0, capacity);
            }
        }
    };

    const bool key_decoded = decode(key_text, PrivateKeyMax, key, key_length);
    Wipe wipe_key { key, key_text ? ((std::strlen(key_text) + 3) / 4) * 3 : 0 };
    if (!key_decoded || !key_length) {
        error = "Private key: missing, not base64 or too long";
        return false;
    }

    const ByteSpan dac_span(dac.get(), dac_length);
    const ByteSpan pai_span(pai.get(), pai_length);

    if (Crypto::VerifyAttestationCertificateFormat(dac_span, Crypto::AttestationCertType::kDAC) != CHIP_NO_ERROR) {
        error = "DAC: not a Matter device attestation certificate (X.509 DER, P-256, VID and PID in the subject)";
        return false;
    }

    if (Crypto::VerifyAttestationCertificateFormat(pai_span, Crypto::AttestationCertType::kPAI) != CHIP_NO_ERROR) {
        error = "PAI: not a Matter product attestation intermediate certificate (X.509 DER, P-256)";
        return false;
    }

    Crypto::AttestationCertVidPid vidpid;
    if ((Crypto::ExtractVIDPIDFromX509Cert(dac_span, vidpid) != CHIP_NO_ERROR)
        || !vidpid.mVendorId.HasValue() || !vidpid.mProductId.HasValue())
    {
        error = "DAC: no VID and PID in the subject";
        return false;
    }

    if (verify_issued(dac_span, pai_span) != CHIP_NO_ERROR) {
        error = "The DAC is not signed by this PAI";
        return false;
    }

    if (declaration_length) {
        Der message;
        if (!der(declaration.get(), declaration_length, message) || (message.tag != 0x30)
            || (message.total != declaration_length))
        {
            error = "Certification declaration: not a CMS message (DER)";
            return false;
        }
    }

    static Custom made;
    made = Custom{};
    struct WipeMade {
        ~WipeMade() {
            std::memset(&made, 0, sizeof(made));
        }
    } wipe_made;

    uint8_t secret[Crypto::kP256_PrivateKey_Length];
    const bool parsed = private_key(key.get(), key_length, secret);
    if (parsed) {
        std::memcpy(&made.keypair[Crypto::kP256_PublicKey_Length], secret, sizeof(secret));
    }
    std::memset(secret, 0, sizeof(secret));

    if (!parsed) {
        error = "Private key: not a P-256 key (DER SEC1 or PKCS#8, PEM, or the 32 raw bytes)";
        return false;
    }

    // the key belongs to the DAC: a signature with it checks out with the DAC's public key
    Crypto::P256PublicKey dac_public;
    if (Crypto::ExtractPubkeyFromX509Cert(dac_span, dac_public) != CHIP_NO_ERROR) {
        error = "DAC: no P-256 public key";
        return false;
    }
    std::memcpy(made.keypair, dac_public.ConstBytes(), Crypto::kP256_PublicKey_Length);

    {
        static constexpr uint8_t Probe[] = "ESPurna custom Matter identity";

        Crypto::P256SerializedKeypair serialized;
        std::memcpy(serialized.Bytes(), made.keypair, sizeof(made.keypair));
        serialized.SetLength(sizeof(made.keypair));

        Crypto::P256Keypair keypair;
        auto err = keypair.Deserialize(serialized);
        std::memset(serialized.Bytes(), 0, serialized.Capacity());

        Crypto::P256ECDSASignature signature;
        if (err == CHIP_NO_ERROR) {
            err = keypair.ECDSA_sign_msg(Probe, sizeof(Probe), signature);
        }
        if (err == CHIP_NO_ERROR) {
            err = dac_public.ECDSA_validate_msg_signature(Probe, sizeof(Probe), signature);
        }

        if (err != CHIP_NO_ERROR) {
            error = "The private key does not belong to the DAC";
            return false;
        }
    }

    made.magic = CustomMagic;
    made.vendor_id = static_cast<uint16_t>(vidpid.mVendorId.Value());
    made.product_id = vidpid.mProductId.Value();
    made.dac_length = static_cast<uint16_t>(dac_length);
    made.pai_length = static_cast<uint16_t>(pai_length);
    made.declaration_length = static_cast<uint16_t>(declaration_length);
    Sha1(dac.get(), dac_length, made.dac_sha1);

    // the header goes invalid first: a write that stops half way leaves no custom identity (the own one is used), never
    // a header with the certificates of another one
    Custom invalid {};
    if (!store_write(KeyCustom, reinterpret_cast<const uint8_t*>(&invalid), sizeof(invalid))
        || !store_write(KeyCustomDac, dac.get(), dac_length)
        || !store_write(KeyCustomPai, pai.get(), pai_length)
        || (declaration_length && !store_write(KeyCustomDeclaration, declaration.get(), declaration_length))
        || !store_write(KeyCustom, reinterpret_cast<const uint8_t*>(&made), sizeof(made)))
    {
        custom_present = false;
        error = "The key-value store of the chip failed";
        return false;
    }

    custom_present = true;
    return true;
}

bool load(uint32_t& passcode, uint16_t& discriminator) {
    if (source == Source::None) {
        // setup() did not run (it always does from the module setup): the automatic choice
        auto wanted = chosen;
        if (wanted == Source::None) {
            wanted = has_factory() ? Source::Factory : Source::Own;
        }

        if (wanted == Source::Factory) {
            source = Source::Factory;
        } else if (read_own() || (make() == CHIP_NO_ERROR)) {
            // the custom identity has the own setup passcode and discriminator
            source = wanted;
        } else {
            return false;
        }
    }

    if (source == Source::Factory) {
        return false;
    }

    passcode = stored.passcode;
    discriminator = stored.discriminator;
    return true;
}

bool install() {
    if (installed) {
        return true;
    }

    uint32_t passcode;
    uint16_t discriminator;
    load(passcode, discriminator);

    switch (source) {
    case Source::None:
        return false;

    case Source::Factory:
        instance_info.base(GetDeviceInstanceInfoProvider());
        SetDeviceInstanceInfoProvider(&instance_info);
        Credentials::SetDeviceAttestationCredentialsProvider(&factory_attestation);
        break;

    case Source::Own:
        instance_info.base(GetDeviceInstanceInfoProvider());
        SetDeviceInstanceInfoProvider(&instance_info);
        SetCommissionableDataProvider(&own_commissionable);
        Credentials::SetDeviceAttestationCredentialsProvider(&own_attestation);
        break;

    case Source::Custom:
        instance_info.base(GetDeviceInstanceInfoProvider());
        SetDeviceInstanceInfoProvider(&instance_info);
        SetCommissionableDataProvider(&own_commissionable);
        Credentials::SetDeviceAttestationCredentialsProvider(&custom_attestation);
        break;
    }

    installed = true;
    return true;
}

bool factory() {
    return source == Source::Factory;
}

espurna::string status() {
    espurna::string out;

    switch ((source != Source::None) ? source : chosen) {
    case Source::None:
        return "SDK test codes and DAC";

    case Source::Factory:
        out = "factory identity";
        break;

    case Source::Own: {
        out = "own identity (development DAC made on the board)";
        if (stored.magic == Magic) {
            out += ", serial ";
            static constexpr char Hex[] = "0123456789abcdef";
            for (const auto byte : stored.serial) {
                out += Hex[byte >> 4];
                out += Hex[byte & 0xf];
            }
        }
        break;
    }

    case Source::Custom: {
        char text[96];
        snprintf_P(text, sizeof(text), PSTR("custom identity (uploaded DAC), VID 0x%04X PID 0x%04X, %s declaration"),
            custom.vendor_id, custom.product_id, custom.declaration_length ? "uploaded" : "SDK example");
        out = text;
        break;
    }
    }

    if (fallback) {
        out += ", the chosen one is not available";
    }

    if ((source != Source::None) && (source != Source::Factory) && !installed) {
        out += ", not installed";
    }

    return out;
}

} // namespace identity
} // namespace matter
} // namespace espurna

// The library calls it before its server init on both chips (matter-espurna matter_app.cpp): the providers of the
// board's identity, in the Matter task (made there on the first start, below the loop's priority). It was defined in
// the Beken backend only: the ESP32 got the library's empty one and ran on the SDK test providers, its PAI request
// failed every commissioning (10.0.5.66 2026-11-14: 'SendPAICertificateRequest' IM 0x01 FAILURE)
extern "C" void matter_identity_install(void) {
    espurna::matter::heap_step("identity");
    espurna::matter::identity::install();
    espurna::matter::heap_step("identity done");
}

#endif // MATTER_SUPPORT
