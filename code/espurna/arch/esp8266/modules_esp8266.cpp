/*

PROTOTYPE: what the ESP8266 on ESP8266_RTOS_SDK gives the module loader (modules_core.h; the loader is modules.cpp;
docs/modules.md)

Flash: the 'factory' partition of arch/esp8266/partitions.csv holds the head at its start and the modules behind it up
to the directory sector (0xF9000, its second copy 0xF8000), under the settings in sectors 250/251 - the same places as
the NONOS build had. A module runs from the flash the cache maps at 0x40200000 + offset (32-bit reads; the SDK's
LoadStoreError handler takes byte reads, slowly), the code and the read-only data in that one view.
RAM: the heap block only, as on ESP32 (one model for the FreeRTOS chips): the IRAM tail of the NONOS build is not
taken, IRAM_ATTR code of a module runs from flash.

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT

#include "espurna_ota.h"
#include "modules_core.h"
#include "rbl_esp8266.h"

#include <esp_ota_ops.h>
#include <esp_system.h>
#include <spi_flash.h>

#include <cstring>

// heap the head keeps for itself (Wi-Fi, the web server) when the Apply sizes the modules' heap block
#ifndef MODULES_HEAP_KEEP
#define MODULES_HEAP_KEEP 4096
#endif

namespace espurna {
namespace modules {
namespace {

// the cache maps the first 1 MB of the flash here
constexpr uint32_t FlashMapped { 0x40200000 };

// the directory: the sector under the settings (250/251, storage_flash_esp8266.cpp), its second copy under it
constexpr uint32_t DirectoryOffset { 0xF9000 };

struct Region {
    uint32_t start { 0 };
    uint32_t lo { 0 };
    uint32_t hi { 0 };
    bool valid { false };
};

Region region;

bool findArea() {
    const auto* running = esp_ota_get_running_partition();
    const uint32_t head = espurna::core::chip::sketch_size();
    if (!running || !head) {
        return false;
    }

    region.start = running->address;
    region.lo = roundUp(running->address + head, SectorSize);
    region.hi = DirectoryOffset;
    region.valid = (region.hi > region.lo) && (region.hi <= (running->address + running->size));
    return region.valid;
}

bool areaReady() {
    static bool tried { false };
    if (!tried) {
        tried = true;
        if (!findArea()) {
            DEBUG_MSG_P(PSTR("[MODULES] No module area\n"));
        }
    }

    return region.valid;
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

// the flash over 1 MB of a bigger chip (core_esp8266.cpp: its size from the JEDEC id) without its last 64 KB: there the
// 4 MB layout of the old NONOS builds kept the settings (sectors 1012-1019, carried over once) and the SDK its own
// sectors. The cache maps the first MB alone: data modules and the data of the modules only (regress-todo.md item 22)
constexpr uint32_t UpperStart { 0x100000 };
constexpr uint32_t UpperTail { 0x10000 };

// the Flash meter from the start of 'factory' (the head) to the directory
Area area() {
    if (!areaReady()) {
        return Area{0, 0, 0};
    }

    Area out{region.start, region.lo, region.hi};

    // the RBL header sector (rbl_esp8266.h: under the directory, the modules under it): every head OTA erases it and
    // writes the new header there, the bootloader its swap state, the head's confirmation erases it. Never a module's:
    // 22.11 an Apply put the tail of the 'tools' slot there - the OTA broke 'tools' ("ranges"), the Apply after the
    // install wrote over the swap state (the new head never confirmed itself, no rollback left)
    out.keep[0] = Area::Range{ESPURNA_RBL_HEADER_AT, ESPURNA_RBL_SECTOR};

    const uint32_t chip = spi_flash_get_chip_size();
    // the package of a new head takes the top ESPURNA_RBL_TOP_ZONE under the tail (rbl_esp8266.h, item 22 stage 2)
    if (chip >= (UpperStart + UpperTail + ESPURNA_RBL_TOP_ZONE + SectorSize)) {
        out.upper = Area::Range{UpperStart, chip - UpperTail - ESPURNA_RBL_TOP_ZONE - UpperStart};
    }

    return out;
}

bool rawRead(uint32_t offset, void* data, size_t size) {
    return spi_flash_read(offset, data, size) == ESP_OK;
}

bool rawErase(uint32_t offset, uint32_t size) {
    return spi_flash_erase_range(offset, size) == ESP_OK;
}

bool rawWrite(uint32_t offset, const void* data, size_t size) {
    return spi_flash_write(offset, data, size) == ESP_OK;
}

// over 1 MB a view the cache does not map (0x40300000 on): a data module's addresses there are read with viewRead()
uint32_t flashView(uint32_t slot) {
    return FlashMapped + slot;
}

// the bytes of a view: through the cache in the first MB, over it with the flash API
bool viewRead(const void* view, void* out, size_t size) {
    const auto at = reinterpret_cast<uintptr_t>(view);
    if ((at >= (FlashMapped + UpperStart)) && (at < (FlashMapped + 0x1000000))) {
        return rawRead(at - FlashMapped, out, size);
    }

    memcpy_P(out, view, size);
    return true;
}

// the code runs from the data view
uint32_t codeView(uint32_t) {
    return 0;
}

// heap only, as on ESP32
void pools(Pools& out) {
    const uint32_t free = esp_get_free_heap_size();
    out.heap = heapBlockSize() + ((free > MODULES_HEAP_KEEP) ? (free - MODULES_HEAP_KEEP) : 0);
    out.fixed = 0;
    out.fixed_base = 0;
    out.fixed_words = false;
    out.code_in_fixed = false;
    out.words = true;
    out.name = nullptr;
}

void statusJson(espurna::string& out) {
    jsonField(out, "chip", spi_flash_get_chip_size());
}

void listHead(::espurna::Output& out) {
    out.printf_P(PSTR("esp8266: factory 0x%06x, modules 0x%06x..0x%06x, map %08x\n"),
        region.start, region.lo, region.hi, FlashMapped);
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT
