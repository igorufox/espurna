/*

PROTOTYPE: what the ESP8266 gives the module loader (modules_core.h; the loader is modules.cpp; docs/modules.md)

Flash: modules lie under the directory sector (the sector under the settings), placed from the top down; the head
grows from 0x0 up. A module runs from the flash mapped at 0x40200000, 32-bit reads only. The IRAM after the head's code
(48 KB IRAM, 16 KB flash cache) is the fixed pool: the Apply puts the IRAM code of the modules at its start, then their
statics, largest first (32-bit access only: the core's non32xfer handler emulates bytes, not from an interrupt, so a
module whose statics take byte access from an interrupt goes to the heap block).

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT && defined(ESP8266)

#include "espurna_ota.h"
#include "storage_eeprom.h"
#include "modules_core.h"

#include <Updater.h>
#include <spi_flash.h>
#include <umm_malloc/umm_malloc.h>

#include <cstring>

// heap the head keeps for itself (Wi-Fi, the web server) when the Apply sizes the modules' heap block
#ifndef MODULES_HEAP_KEEP
#define MODULES_HEAP_KEEP 4096
#endif

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t FlashMapped { 0x40200000 };

// the IRAM after the head's code, with the 48 KB IRAM layout (flash cache 16 KB)
extern "C" void _text_end(void);

uint32_t iramTailStart() {
    return (reinterpret_cast<uint32_t>(&_text_end) + 32 + 7) & ~7u;
}

uint32_t iramTailSize() {
#if defined(MMU_IRAM_SIZE) && (MMU_IRAM_SIZE > 0x8000) && defined(NON32XFER_HANDLER)
    const uint32_t end = 0x40100000 + MMU_IRAM_SIZE;
    return (end > iramTailStart()) ? (end - iramTailStart()) : 0;
#else
    return 0;
#endif
}

// the sector under the settings: EEPROM_Rotate keeps them in its base sector and the pool under it (251 and 250 on a
// 1 MB chip, 8 sectors on 4 MB); the directory at 0xFA000 was in that pool and the settings writes erased it
uint32_t directoryOffset() {
    static uint32_t offset { 0 };
    if (!offset) {
        auto& eeprom = eepromInstance();
        offset = (eeprom.base() - eeprom.size()) * SectorSize;
    }

    return offset;
}

uint32_t headEnd() {
    return roundUp(ESP.getSketchSize(), SectorSize);
}

// --- settings of the old 4 MB layout -------------------------------------------------------------------------------
// EEPROM_Rotate keeps the settings in a pool of sectors under the SDK's 4 at the end of the flash the image header
// names: the 1 MB head reads sectors 251 and 250, an image of the 4 MB layout wrote 1019..1012 of the real chip. A
// sector is a copy when its sum of bytes (but the CRC and the counter at EepromRotateOffset) is the stored CRC; the
// newest copy has the highest counter (mod 256). Copied once: only when the head's own pool has no valid copy. The SDK
// refuses reads past the size in the image header (1 MB): the size is raised for the read

bool readPastHeader(uint32_t address, uint32_t* data, size_t size) {
    const uint32_t real = ESP.getFlashChipRealSize();
    if ((address + size) > real) {
        return false;
    }

    const uint32_t saved = flashchip->chip_size;
    flashchip->chip_size = real;
    const bool ok = (spi_flash_read(address, data, size) == SPI_FLASH_RESULT_OK);
    flashchip->chip_size = saved;
    return ok;
}

uint16_t settingsCrc(const uint8_t* data) {
    uint16_t crc = 0;
    for (size_t address = 0; address < EepromSize; ++address) {
        if ((address >= EepromRotateOffset) && (address <= EepromRotateOffset + 2)) {
            continue;
        }
        crc += data[address];
    }
    return crc;
}

bool settingsValid(const uint8_t* data) {
    const uint16_t stored = (data[EepromRotateOffset] << 8) | data[EepromRotateOffset + 1];
    return stored == settingsCrc(data);
}

size_t settingsPool(uint32_t last) {
    return (last > 4000) ? 32 : (last > 2000) ? 16 : (last > 1000) ? 8 : (last > 500) ? 4 : 2;
}

} // namespace
} // namespace modules
} // namespace espurna

void modulesMigrateSettings() {
    using namespace espurna::modules;
    const uint32_t real = ESP.getFlashChipRealSize();
    const uint32_t mapped = ESP.getFlashChipSize();
    if (real <= mapped) {
        return;
    }

    auto* data = static_cast<uint8_t*>(malloc(EepromSize));
    if (!data) {
        return;
    }

    // the head's pool first: any valid copy there, nothing to do
    const uint32_t head_last = (mapped / SectorSize) - 5;
    for (size_t index = 0; index < settingsPool(head_last); ++index) {
        if (ESP.flashRead((head_last - index) * SectorSize, reinterpret_cast<uint32_t*>(data), EepromSize)
            && settingsValid(data))
        {
            free(data);
            return;
        }
    }

    // the newest valid copy of the old layout's pool
    const uint32_t old_last = (real / SectorSize) - 5;
    bool found = false;
    uint32_t best_sector = 0;
    uint8_t best_value = 0;
    for (size_t index = 0; index < settingsPool(old_last); ++index) {
        const uint32_t sector = old_last - index;
        if (!readPastHeader(sector * SectorSize, reinterpret_cast<uint32_t*>(data), EepromSize) || !settingsValid(data)) {
            continue;
        }
        const uint8_t value = data[EepromRotateOffset + 2];
        const bool newer = !found || ((value < best_value) && ((best_value - value) > 128))
            || ((value > best_value) && ((value - best_value) < 128));
        if (newer) {
            found = true;
            best_sector = sector;
            best_value = value;
        }
    }

    if (found && readPastHeader(best_sector * SectorSize, reinterpret_cast<uint32_t*>(data), EepromSize)
        && ESP.flashEraseSector(head_last))
    {
        ESP.flashWrite(head_last * SectorSize, reinterpret_cast<const uint32_t*>(data), EepromSize);
    }

    free(data);
}

namespace espurna {
namespace modules {
namespace arch {

// the SDK reserve and the TCP accept gate (heap_reserve_esp8266.cpp)
void setup() {
    modulesHeapSetup();
}

// the Flash meter from 0: the head, then the modules up to the directory
Area area() {
    return Area{0, headEnd(), directoryOffset()};
}

bool rawRead(uint32_t offset, void* data, size_t size) {
    return ESP.flashRead(offset, static_cast<uint8_t*>(data), size);
}

bool rawErase(uint32_t offset, uint32_t size) {
    for (uint32_t done = 0; done < size; done += SectorSize) {
        if (!ESP.flashEraseSector((offset + done) / SectorSize)) {
            return false;
        }
    }

    return true;
}

// the callers' buffers are word aligned
bool rawWrite(uint32_t offset, const void* data, size_t size) {
    return ESP.flashWrite(offset, static_cast<const uint32_t*>(data), size);
}

// MD5 of the running sketch, as the module link took it from the head .bin
bool headId(uint8_t out[16]) {
    return hexId(ESP.getSketchMD5(), out);
}

uint32_t flashView(uint32_t slot) {
    return FlashMapped + slot;
}

// the code runs from the data view, IRAM code from the fixed pool
uint32_t codeView(uint32_t) {
    return 0;
}

void pools(Pools& out) {
    // the free heap the loop sees (not inside this request, whose buffers take a part of it): the block comes out of
    // it at the next boot, the head keeps MODULES_HEAP_KEEP of it
    const uint32_t free = modulesHeapLoop();
    out.heap = heapBlockSize() + ((free > MODULES_HEAP_KEEP) ? (free - MODULES_HEAP_KEEP) : 0);
    out.fixed = iramTailSize();
    out.fixed_base = iramTailStart();
    out.fixed_words = true;
    out.code_in_fixed = true;
    out.words = true;
    out.name = "IRAM";
}

void statusJson(String& out) {
    jsonField(out, F("reserve"), modulesHeapReserve());
    jsonField(out, F("reserve_used"), modulesHeapReserveUsed());
    jsonField(out, F("refused"), modulesAcceptRefused());
    // the free heap the loop sees, outside this request ("heap" is measured inside it)
    jsonField(out, F("heap_loop"), modulesHeapLoop());
    jsonField(out, F("heap_low"), modulesHeapLow());
    jsonField(out, F("chip"), ESP.getFlashChipRealSize());
}

uint32_t failedAllocations() {
    return umm_get_oom_count();
}

// the web upload, and ArduinoOTA (Update) too
bool otaBusy() {
    return otaWebUploading() || Update.isRunning();
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT && ESP8266
