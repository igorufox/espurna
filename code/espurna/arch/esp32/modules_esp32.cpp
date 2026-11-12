/*

PROTOTYPE: what the ESP32 gives the module loader (modules_core.h; the loader is modules.cpp; docs/modules.md, "ESP32")

Flash: the modules lie in the tail of app0 behind the head (or in a data partition named `modules`), placed from the top
down under the directory (the last sector). The whole area is mapped twice at boot (arch::setup, before anything else
of ESPurna maps flash for good), into the instruction bus (code, kind 3) and the data bus (header, rodata, the .data
image, kind 1), and the maps stay. When they come out elsewhere at a boot the core relocates the modules again.

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT

#include "modules_core.h"

#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>

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
    region.hi = app->address + app->size - SectorSize;
    return region.hi > region.lo;
}

bool mapArea() {
    const void* ptr { nullptr };
    spi_flash_mmap_handle_t handle;
    const auto offset = region.lo - region.part->address;
    const auto size = region.hi - region.lo;
    if (esp_partition_mmap(region.part, offset, size, ESP_PARTITION_MMAP_INST, &ptr, &handle) != ESP_OK) {
        return false;
    }
    region.irom = reinterpret_cast<uint32_t>(ptr);

    if (esp_partition_mmap(region.part, offset, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
        return false;
    }
    region.drom = reinterpret_cast<uint32_t>(ptr);

    region.mapped = true;
    return true;
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

// the Flash meter from the start of app0 (the head) to the directory
Area area() {
    if (!areaReady()) {
        return Area{0, 0, 0};
    }

    return Area{region.start, region.lo, region.hi};
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

const char* imageBegin(Upload&) {
    return region.mapped ? nullptr : PSTR("no module area mapped");
}

// the cached lines of the maps of the slot: the old words otherwise
void imageDone(uint32_t slot, uint32_t size) {
    spi_flash_check_and_flush_cache(slot, size);
}

// heap only: RTC FAST is reachable from the PRO CPU alone (the loop runs on the APP CPU), the IRAM tail takes 32-bit
// access only and holds no IRAM_ATTR code of modules here
void pools(Pools& out) {
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out.heap = heapBlockSize() + ((largest > MODULES_HEAP_KEEP) ? (largest - MODULES_HEAP_KEEP) : 0);
    out.fixed = 0;
    out.fixed_base = 0;
    out.fixed_words = false;
    out.code_in_fixed = false;
    out.words = false;
    out.name = nullptr;
}

void statusJson(String& out) {
    jsonField(out, F("chip"), ESP.getFlashChipSize());
}

void listHead(Print& out) {
    out.printf_P(PSTR("esp32: %s 0x%06x, maps: irom %08x, drom %08x%s\n"),
        region.part ? region.part->label : "-", region.part ? region.part->address : 0,
        region.irom, region.drom, region.mapped ? "" : " (not mapped)");
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT && ESP32
