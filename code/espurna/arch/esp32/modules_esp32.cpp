/*

PROTOTYPE: what the ESP32 gives the module loader (modules_core.h; the loader is modules.cpp; docs/modules.md, "ESP32")

Flash: the modules lie in the tail of app0 behind the head (or in a data partition named `modules`), placed from the top
down under the directory (the last sector). The whole area is mapped twice at boot (arch::setup, before anything else
of ESPurna maps flash for good), into the instruction bus (code, kind 3) and the data bus (header, rodata, the .data
image, kind 1), and the maps stay. When they come out elsewhere at a boot the core relocates the modules again.

*/

#include "../../espurna.h"
#include "../../modules.h"

#if MODULES_SUPPORT

#include "../../modules_core.h"

#include "rbl_esp32.h"

#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>
#include <esp_mmu_map.h>

#include <algorithm>

// arch/esp32/modules_pool_esp32.c: the IRAM pool of the modules (package version 7: their pool code)
extern "C" uint32_t modulesPoolBase();
extern "C" uint32_t modulesPoolEnd();

// esp_private/cache_utils.h: drops the cached lines of the mapped pages of a flash range (a module relocated in place)
extern "C" bool spi_flash_check_and_flush_cache(size_t start_addr, size_t length);

// heap the head keeps for itself (Wi-Fi, the web server, TLS) when the Apply sizes the modules' heap block
#ifndef MODULES_HEAP_KEEP
#define MODULES_HEAP_KEEP 32768
#endif

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t PageSize { 0x10000 };

bool readFlash(uint32_t address, void* data, uint32_t size) {
    return esp_flash_read(nullptr, data, address, size) == ESP_OK;
}

struct Region {
    const esp_partition_t* part { nullptr };    // app0 (its tail) or `modules`
    uint32_t start { 0 };       // flash address where the Flash meter starts (app0: the head)
    uint32_t lo { 0 };          // first byte for modules (64 KB aligned: the start of the maps)
    uint32_t hi { 0 };          // the directory
    uint32_t tail { 0 };        // where the tail behind the head starts: the initial image's modules file is there
    uint32_t irom { 0 };        // instruction view of `lo`
    uint32_t drom { 0 };        // data view of `lo`
    bool mapped { false };
};

Region region;

// end of the app image in app0, from its segment headers (esp_image_verify() would read and hash all of it)
uint32_t headSize(const esp_partition_t* app) {
    esp_image_header_t header;
    if (!readFlash(app->address, &header, sizeof(header))
        || (header.magic != ESP_IMAGE_HEADER_MAGIC) || (header.segment_count > ESP_IMAGE_MAX_SEGMENTS))
    {
        return 0;
    }

    uint32_t pos = sizeof(header);
    for (uint8_t index = 0; index < header.segment_count; ++index) {
        esp_image_segment_header_t segment;
        if (!readFlash(app->address + pos, &segment, sizeof(segment))) {
            return 0;
        }
        pos += sizeof(segment) + segment.data_len;
    }

    // checksum byte at the end of a 16 byte block, then the SHA-256 when appended
    pos = roundUp(pos + 1, 16);
    if (header.hash_appended) {
        pos += 32;
    }

    return pos;
}

bool findArea() {
    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "modules");
    if (part) {
        region.part = part;
        region.start = part->address;
        region.lo = part->address;
        region.hi = part->address + part->size - SectorSize;
        return region.hi > region.lo;
    }

    const auto* app = esp_ota_get_running_partition();
    const auto head = app ? headSize(app) : 0;
    if (!head) {
        return false;
    }

    region.part = app;
    region.start = app->address;
    region.lo = roundUp(app->address + head, PageSize);
    region.tail = region.lo;
    region.hi = app->address + app->size - SectorSize;
    return region.hi > region.lo;
}

// The area is the part of app0 behind the head that both buses can map: the instruction window (0x400D0000..
// 0x40400000) and the data window are shared with the head's own maps, and app0 (3712 KB since the layout without
// safeboot, 2026-11-18) is bigger than what is left of them (22.11: the whole tail did not map, "esp_mmu_map: no such
// vaddr range", no module was ever installed). So the area is the top of the tail, as much as maps, in pages; the
// modules sit at its top and the directory above it.
bool mapArea() {
    const auto hi = region.hi;

    // the longest free run of each window: start there instead of failing map after map
    size_t inst_free { 0 };
    size_t data_free { 0 };
    auto lo = region.lo;
    if ((esp_mmu_map_get_max_consecutive_free_block_size(MMU_MEM_CAP_EXEC, MMU_TARGET_FLASH0, &inst_free) == ESP_OK)
        && (esp_mmu_map_get_max_consecutive_free_block_size(MMU_MEM_CAP_READ, MMU_TARGET_FLASH0, &data_free) == ESP_OK))
    {
        const uint32_t fits = std::min(inst_free, data_free) & ~(PageSize - 1);
        // lo is the start of a map page (the bases of the modules are page offsets of both views)
        if (fits < (hi - lo)) {
            lo = roundUp(hi - fits, PageSize);
        }
    }

    for (; (lo + PageSize) <= hi; lo += PageSize) {
        const auto offset = lo - region.part->address;
        const auto size = hi - lo;

        const void* inst { nullptr };
        spi_flash_mmap_handle_t inst_handle;
        if (esp_partition_mmap(region.part, offset, size, ESP_PARTITION_MMAP_INST, &inst, &inst_handle) != ESP_OK) {
            continue;
        }

        const void* data { nullptr };
        spi_flash_mmap_handle_t data_handle;
        if (esp_partition_mmap(region.part, offset, size, ESP_PARTITION_MMAP_DATA, &data, &data_handle) != ESP_OK) {
            spi_flash_munmap(inst_handle);
            continue;
        }

        if (lo != region.lo) {
            DEBUG_MSG_P(PSTR("[MODULES] Area 0x%08X..0x%08X (%u KB): what the flash maps can take, the tail starts at 0x%08X\n"),
                lo, hi, (hi - lo) / 1024, region.lo);
        }

        region.lo = lo;
        region.irom = reinterpret_cast<uint32_t>(inst);
        region.drom = reinterpret_cast<uint32_t>(data);
        region.mapped = true;
        return true;
    }

    return false;
}

// esp_flash_write() aborts on any write into the running app partition (DANGEROUS_WRITE_ABORTS of the Arduino
// sdkconfig). Same chip, without that check; the core writes only inside the module area and the directory sector
esp_flash_os_functions_t unprotected_os;
esp_flash_t unprotected_chip;

esp_flash_t* writableChip() {
    static bool done { false };
    if (!done) {
        unprotected_os = *esp_flash_default_chip->os_func;
        unprotected_os.region_protected = nullptr;
        unprotected_chip = *esp_flash_default_chip;
        unprotected_chip.os_func = &unprotected_os;
        done = true;
    }

    return &unprotected_chip;
}

} // namespace

namespace arch {

} // namespace arch

namespace {

// the first call finds the area and maps it: modulesRamSetup() reads the directory before arch::setup() runs, and
// both maps are taken before anything else of ESPurna maps flash for good
bool areaReady() {
    static bool tried { false };
    if (!tried) {
        tried = true;
        if (!findArea() || !mapArea()) {
            DEBUG_MSG_P(PSTR("[MODULES] No module area\n"));
        }
    }

    return region.part != nullptr;
}

} // namespace

namespace arch {

void setup() {
    areaReady();
}

// the flash API takes every offset of the chip
uint32_t flashReach() {
    return 0xFFFFFFFF;
}

// the Flash meter from the start of app0 (the head) to the directory
Area area() {
    if (!areaReady()) {
        return Area{0, 0, 0};
    }

    Area out{region.start, region.lo, region.hi};
    if (region.tail && (region.tail != region.lo)) {
        out.initial = region.tail;
    }

    // the RBL header sector of the head OTA under the directory (rbl_esp32.h): the OTA erases it and writes the header
    // there, the bootloader its swap state, the head's confirmation erases it - never a module's (the ESP8266's bug of
    // 22.11: the module there was broken by the OTA, the swap state by the Apply)
    if ((ESPURNA_RBL_HEADER_AT >= region.lo) && (ESPURNA_RBL_HEADER_AT < region.hi)) {
        out.keep[0] = Area::Range{ESPURNA_RBL_HEADER_AT, ESPURNA_RBL_SECTOR};
    }

    return out;
}

bool rawRead(uint32_t offset, void* data, size_t size) {
    return readFlash(offset, data, size);
}

bool rawErase(uint32_t offset, uint32_t size) {
    return esp_flash_erase_region(writableChip(), offset, size) == ESP_OK;
}

bool rawWrite(uint32_t offset, const void* data, size_t size) {
    return esp_flash_write(writableChip(), data, offset, size) == ESP_OK;
}

uint32_t flashView(uint32_t slot) {
    return region.mapped ? (region.drom + (slot - region.lo)) : 0;
}

uint32_t codeView(uint32_t slot) {
    return region.mapped ? (region.irom + (slot - region.lo)) : 0;
}

// the data view of the area reads any byte (DROM), the data of the modules lies in it under the directory sector
uint32_t dataView(uint32_t offset) {
    return (region.mapped && (offset >= region.lo) && (offset < region.hi)) ? (region.drom + (offset - region.lo)) : 0;
}

const char* imageBegin(Upload&) {
    return region.mapped ? nullptr : PSTR("no module area mapped");
}

// the cached lines of the maps of the slot: the old words otherwise
void imageDone(uint32_t slot, uint32_t size) {
    spi_flash_check_and_flush_cache(slot, size);
}

// the statics in the heap block (RTC FAST is reachable from the PRO CPU alone, the loop runs on the APP CPU); the fixed
// pool: the IRAM behind the head's code (modules_pool_esp32.c), the pool code of the modules only (Header::iram_*:
// IRAM_ATTR, the BT controller's ISR), copied there with 32-bit stores at their start. No statics there: the IRAM
// takes no byte access at all
void pools(Pools& out) {
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out.heap = heapBlockSize() + ((largest > MODULES_HEAP_KEEP) ? (largest - MODULES_HEAP_KEEP) : 0);
    out.fixed_base = modulesPoolBase();
    out.fixed = modulesPoolEnd() - out.fixed_base;
    out.fixed_words = true;
    out.code_in_fixed = true;
    out.code_only = true;
    out.words = false;
    out.name = PSTR("IRAM");
}

void statusJson(espurna::string& out) {
    jsonField(out, "chip", ::espurna::core::chip::flash_size());
}

void listHead(::espurna::Output& out) {
    out.printf_P(PSTR("esp32: %s 0x%06x, maps: irom %08x, drom %08x%s\n"),
        region.part ? region.part->label : "-", region.part ? region.part->address : 0,
        region.irom, region.drom, region.mapped ? "" : " (not mapped)");
}

} // namespace arch
} // namespace modules
} // namespace espurna

// The IRAM pool (modules_pool_esp32.c, _iram_end .. 0x400A0000) is the modules' alone: no part of it goes to the heap.
// 22.11, the first run of the RTOS head (DevKit COM3): app_main() gave the pool past the installed pool code to the heap
// (MALLOC_CAP_EXEC | 32BIT), and an Apply then wrote the pool code of new modules into what the heap owned; with it
// the board crashed in the scheduler (vTaskSwitchContext, a PS word in a ready list), in coex of the Wi-Fi task, hung
// in a spinlock - within seconds of an upload, minutes idle; without it 15 minutes idle with the modules on, clean.

#endif // MODULES_SUPPORT && ESP32
