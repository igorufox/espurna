/*

PROTOTYPE: installable modules of the ESP8266 1 MB head image (docs/modules.md)

The head (MODULES_SUPPORT) keeps a directory of modules in the last sector of the sketch area, places an uploaded
module package at a free flash slot (adding the slot / pool bases to the relocated words as they arrive) and at boot
starts every module made for its own image (MD5 of the running sketch).

A module (MODULE_<NAME>, scripts/esp8266_module_link.py) is linked against the head's firmware.elf: it calls the head
directly; its code, PSTR strings and vtables run from its flash slot, its .data / .rodata / .bss live in the head's
RAM pool, its IRAM functions in the head's IRAM pool.

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace modules {

// first bytes of a module in flash (and of the module image inside a package). Addresses are absolute, the
// module link puts its placeholder addresses here and the head relocates them with the rest of the module
static constexpr uint32_t HeaderMagic { 0x31444D45 }; // "EMD1"
static constexpr uint32_t HeaderVersion { 1 };

struct Header {
    uint32_t magic;
    uint32_t version;
    char name[16];
    uint8_t head_md5[16];   // filled by the link script: MD5 of the head .bin it was linked against
    uint32_t flash_start;   // == address of this header
    uint32_t flash_end;
    uint32_t data_load;     // .data + .rodata image in flash, copied to data_start..data_end at boot
    uint32_t data_start;
    uint32_t data_end;
    uint32_t bss_start;
    uint32_t bss_end;
    uint32_t iram_load;     // IRAM functions in flash, copied to iram_start..iram_end at boot
    uint32_t iram_start;
    uint32_t iram_end;
    uint32_t init_start;    // .ctors of the module, called after the copies
    uint32_t init_end;
    uint32_t entry;         // void entry(), registers the module with the head. 0 - a data module: the bytes after
                            // the header are its data, data_end is their size (not an address), nothing runs
    uint32_t priority;      // modules start in this order, lower first: providers of an API before its users
    char needs[32];         // the modules it needs, by name, comma separated (the link writes it, from the ini;
                            // not `requires`: a C++20 keyword, ESP32 builds with C++20)
};

static_assert(sizeof(Header) == 128, "");

// functions one module provides to the others through the head (modules_api.cpp forwards to the slot the
// provider's entry fills; scripts/esp8266_module_link.py renames the provider's own ones, `provides` in the ini)
enum class Api : size_t {
    MagnitudeCount,
    MagnitudeInfo,
    MagnitudeTypeTopic,
    MagnitudeUnitsName,
    SensorReady,
    SensorWebSocketMagnitudes,
    SensorOnMagnitudeRead,
    SensorOnMagnitudeReport,
    MagnitudeValue,
    MagnitudeReportValue,
    MagnitudeReadValue,
    MagnitudeType,
    MagnitudeIndex,
    MagnitudeTopic,
    DomoticzSendMagnitude,
    TspkEnqueueMagnitude,
    LightStateSet,
    LightStateIdSet,
    LightStateGet,
    LightUpdate,
    LightUpdateTransition,
    LightChannelGet,
    LightChannelSet,
    LightChannels,
    LightOnReport,
    LightColor,
    LightUseRgb,
    LightHasColor,
    LightHasWhite,
    LightHasColdWhite,
    LightHasWarmWhite,
    LightColdWhiteSet,
    LightColdWhiteGet,
    LightWarmWhiteSet,
    LightWarmWhiteGet,
    LightBrightnessSet,
    LightBrightnessGet,
    LightBrightnessPercent,
    LightMiredsRange,
    LightTemperatureMireds,
    LightTransitionStep,
    LightTransitionTime,
    LightHs,
    LightHsvGet,
    LightRgbSet,
    LightRgbGet,
    LightRedSet,
    LightGreenSet,
    LightBlueSet,
    LightBrightnessStep,
    LightBrightnessStepMultiplier,
    LightMakeStateRelayProvider,
    AlexaPause,
    MdnsRunning,
    MdnsServiceQuery,
    UartPort,
    UartClaim,
    UartClaimed,
    UartOnClaim,
    UartOpen,
    UartBaud,
    TlsStart,
    RelayCount,
    RelayStatusGet,
    RelayStatusSet,
    RelayStatusAll,
    RelayToggle,
    RelayParsePayload,
    RelayPayload,
    RelayOnStatusChange,
    RelayOnStatusNotify,
    RelayOnReady,
    RelayBtnGpio,
    ButtonSetCustomAction,
    ApiRegisterBasic,
    ApiRegisterJson,
    NtpOnTick,
    NtpDateTime,
    NtpSynced,
    FanMakeRelayProvider,
    FanSpeed,
    LightfoxMakeRelayProvider,
    LightfoxMakeButtonPin,
    TuyaMakeRelayProvider,
    DisplayOn,
    RfbSend,
    RfbOnCode,
    Count,
};

// upload format, written by the link script (all little endian)
//   PackageHeader
//   per 4 KiB chunk of the module image: 256 byte map (2 bits per 32 bit word, LSB first: 0 - as is,
//   1 - + flash delta, 2 - + RAM delta, 3 - + IRAM delta), then the chunk (the last one may be shorter)
static constexpr uint32_t PackageMagic { 0x31504D45 }; // "EMP1"

struct PackageHeader {
    uint32_t magic;
    uint32_t version;
    char name[16];
    uint8_t head_md5[16];
    uint32_t image_size;    // module image bytes (multiple of 4)
    uint32_t ram_size;      // .data + .rodata + .bss, RAM pool bytes
    uint32_t iram_size;     // IRAM pool bytes
    uint32_t link_flash;    // placeholder bases the module was linked at
    uint32_t link_ram;
    uint32_t link_iram;
    uint32_t crc32;         // over everything after this header (maps + chunks)
    uint32_t links;         // its links to the modules it needs (the branch lists, LinkWord / LinkBranch)
};

static_assert(sizeof(PackageHeader) == 72, "");

static constexpr size_t ChunkSize { 4096 };
static constexpr size_t MapSize { ChunkSize / 4 / 4 };

} // namespace modules
} // namespace espurna

extern "C" void* espurna_module_api[static_cast<size_t>(espurna::modules::Api::Count)];

template <typename T>
inline void modulesProvide(espurna::modules::Api api, T* func) {
    espurna_module_api[static_cast<size_t>(api)] = reinterpret_cast<void*>(func);
}

template <typename T>
inline T* modulesApi(espurna::modules::Api api) {
    return reinterpret_cast<T*>(espurna_module_api[static_cast<size_t>(api)]);
}

// a module (MODULE_BUILD): its header at the start of its flash and its entry. Used once per module source:
//   ESPURNA_MODULE(scheduler, "scheduler", 50) { schSetup(); }
#if MODULE_BUILD
extern "C" {
extern char __mod_flash_start[];
extern char __mod_flash_end[];
extern char __mod_data_load[];
extern char __mod_data_start[];
extern char __mod_data_end[];
extern char __mod_bss_start[];
extern char __mod_bss_end[];
extern char __mod_iram_load[];
extern char __mod_iram_start[];
extern char __mod_iram_end[];
extern char __mod_init_start[];
extern char __mod_init_end[];
}

#define ESPURNA_MODULE(ID, NAME, PRIORITY) \
    extern "C" void espurna_module_entry_##ID(); \
    extern "C" const espurna::modules::Header espurna_module_header_##ID; \
    extern "C" __attribute__((section(".module_header"), used)) \
    const espurna::modules::Header espurna_module_header_##ID { \
        espurna::modules::HeaderMagic, espurna::modules::HeaderVersion, NAME, {}, \
        reinterpret_cast<uint32_t>(__mod_flash_start), reinterpret_cast<uint32_t>(__mod_flash_end), \
        reinterpret_cast<uint32_t>(__mod_data_load), reinterpret_cast<uint32_t>(__mod_data_start), \
        reinterpret_cast<uint32_t>(__mod_data_end), reinterpret_cast<uint32_t>(__mod_bss_start), \
        reinterpret_cast<uint32_t>(__mod_bss_end), reinterpret_cast<uint32_t>(__mod_iram_load), \
        reinterpret_cast<uint32_t>(__mod_iram_start), reinterpret_cast<uint32_t>(__mod_iram_end), \
        reinterpret_cast<uint32_t>(__mod_init_start), reinterpret_cast<uint32_t>(__mod_init_end), \
        reinterpret_cast<uint32_t>(&espurna_module_entry_##ID), PRIORITY }; \
    extern "C" void espurna_module_entry_##ID()

// the setup of a part that came with the module (an address inside it); the head's parts are set up already. The code
// of a module is in its flash (flash_start..flash_end) or in its code range (iram_start..iram_end: the instruction
// view of the slot on ESP32, ref. scripts/esp32_module.ld; the IRAM code on ESP8266), every chip alike
#define ESPURNA_MODULE_OWNS(ID, FUNC) \
    (((reinterpret_cast<uint32_t>(&FUNC) >= espurna_module_header_##ID.flash_start) \
      && (reinterpret_cast<uint32_t>(&FUNC) < espurna_module_header_##ID.flash_end)) \
     || ((reinterpret_cast<uint32_t>(&FUNC) >= espurna_module_header_##ID.iram_start) \
      && (reinterpret_cast<uint32_t>(&FUNC) < espurna_module_header_##ID.iram_end)))
#endif

// the module of that name is installed and switched on (the directory), whether or not it started yet. In the head;
// declared for the module objects too (MODULE_BUILD has MODULES_SUPPORT 0), they call it through the head's exports
bool modulesEnabled(const char* name);

// The flash a module keeps its data in (the data column of ESPURNA_MODULES, config/head.h): whole sectors at the top of
// the module area, placed by the Apply only when the module is installed, kept by the next Apply while it stays (its
// place the same). Offsets from its start, word aligned, lengths whole words; the erase whole sectors (raw flash: an
// erased word reads 0xFFFFFFFF, a write only clears bits). 0 / false - the module has none. In the head, for the modules
uint32_t modulesDataSize(const char* name);
bool modulesDataRead(const char* name, uint32_t at, void* data, size_t len);
bool modulesDataWrite(const char* name, uint32_t at, const void* data, size_t len);
bool modulesDataErase(const char* name, uint32_t at, size_t len);

// the part of the WebUI a module brings (U31): its panels, status lines, templates, styles and script, one gzip'ed
// fragment in the module's flash (html/build/ui-<name>.html.gz, espurna/static/ui_<name>.ipp). The module adds it when
// it starts, the head serves it (GET /ui: the names, GET /ui/part?name=<name>: the fragment) and the page of the
// webui module puts every part in before it connects. In the head, for the modules
void modulesUi(const char* name, const uint8_t* data, size_t size);

#if MODULES_SUPPORT
// first thing after the settings storage: the heap block of the modules' statics, before ESPurna allocates much
void modulesRamSetup();
void modulesBootSetup();
void modulesSetup();
bool modulesRunning();

// the identity of the head (its GNU build id, 32 hex digits): what /modules and `info` call "md5", what a module
// carries of the head it is linked against
String modulesHeadId();
// the modules off at the next boot with this reason (the off mark of the directory; nothing when none is installed or
// the mark is set already): leaving the unstable mode (system.cpp) must not start the modules that put it there
bool modulesOffMark(const char* reason);
// directory erased, modules do not start at the next boot (ArduinoOTA writes over them)
void modulesForget();

// first thing at boot: settings of the old 4 MB layout (pool at the end of the real chip) into the head's pool, once.
// Only the ESP8266 had that layout (arch/esp8266/modules_esp8266.cpp, called by its system_esp8266.cpp)
void modulesMigrateSettings();

// the SDK reserve and the TCP accept gate (arch/esp8266/heap_reserve_esp8266.cpp)
void modulesHeapSetup();
uint32_t modulesHeapReserve();
uint32_t modulesHeapReserveUsed();
uint32_t modulesAcceptRefused();
uint32_t modulesHeapLoop();
uint32_t modulesHeapLow();

// GET / of the head: the WebUI module (a data module named "webui"), or the head's own page
namespace espurna { namespace http { namespace server { class Request; } } }
void modulesOnHome(espurna::http::server::Request&);
#endif
