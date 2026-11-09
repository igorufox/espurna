/*

PROTOTYPE: installable modules of the ESP8266 1 MB head image (docs/esp8266-modules.md)

The head (MODULES_SUPPORT) keeps a directory of modules in the last sector of the sketch area, places an uploaded
module package at a free flash slot (adding the slot / arena bases to the relocated words as they arrive) and at boot
starts every module made for its own image (MD5 of the running sketch).

A module (MODULE_<NAME>, scripts/esp8266_module_link.py) is linked against the head's firmware.elf: it calls the head
directly; its code, PSTR strings and vtables run from its flash slot, its .data / .rodata / .bss live in the head's
RAM arena, its IRAM functions in the head's IRAM arena.

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
};

static_assert(sizeof(Header) == 96, "");

// functions one module provides to the others through the head (modules_api_esp8266.cpp forwards to the slot the
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
    uint32_t ram_size;      // .data + .rodata + .bss, RAM arena bytes
    uint32_t iram_size;     // IRAM arena bytes
    uint32_t link_flash;    // placeholder bases the module was linked at
    uint32_t link_ram;
    uint32_t link_iram;
    uint32_t crc32;         // over everything after this header (maps + chunks)
    uint32_t reserved;
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

// the setup of a part that came with the module (an address inside it); the head's parts are set up already
#define ESPURNA_MODULE_OWNS(ID, FUNC) \
    ((reinterpret_cast<uint32_t>(&FUNC) >= espurna_module_header_##ID.flash_start) \
     && (reinterpret_cast<uint32_t>(&FUNC) < espurna_module_header_##ID.flash_end))
#endif

#if MODULES_SUPPORT
void modulesBootSetup();
void modulesSetup();
bool modulesRunning();
// directory erased, modules do not start at the next boot (ArduinoOTA writes over them)
void modulesForget();

// GET / of the head: the WebUI module (a data module named "webui"), or the head's own page
class AsyncWebServerRequest;
void modulesOnHome(AsyncWebServerRequest*);
#endif
