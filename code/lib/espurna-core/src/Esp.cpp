#include "Esp.h"

#include "core_chip.h"

namespace chip = espurna::core::chip;

EspClass ESP;

uint32_t EspClass::getFreeHeap() {
    return chip::heap_free();
}

uint32_t EspClass::getMaxAllocHeap() {
    return chip::heap_max_alloc();
}

uint32_t EspClass::getFlashChipSize() {
    return chip::flash_size();
}

uint32_t EspClass::getFlashChipMode() {
    return chip::flash_mode();
}

uint32_t EspClass::getFlashChipSpeed() {
    return chip::flash_speed();
}

uint32_t EspClass::getSketchSize() {
    return chip::sketch_size();
}

uint32_t EspClass::getFreeSketchSpace() {
    return chip::sketch_space();
}

uint32_t EspClass::getCycleCount() {
    return chip::cycle_count();
}

uint32_t EspClass::getCpuFreqMHz() {
    return chip::cpu_mhz();
}

const char* EspClass::getSdkVersion() {
    return chip::sdk_version();
}

const char* EspClass::getChipModel() {
    return chip::chip_model();
}

uint8_t EspClass::getChipRevision() {
    return chip::chip_revision();
}

void EspClass::restart() {
    chip::restart();
}
