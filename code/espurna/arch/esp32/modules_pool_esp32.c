/*

The IRAM pool of the modules on the ESP32 (package version 7, owner 2026-11-19 «да, расширяй»): the IRAM behind the
head's code (_iram_end .. the end of SRAM0's instruction bus), kept out of the heap here, so the Apply can place the
pool code of the modules (Header::iram_*, PoolWord) at fixed addresses. modules_esp32.cpp pools() gives it to the
loader. None of it goes to the heap (22.11: giving the unused part to the heap at boot crashed the board, see
modules_esp32.cpp at its end); the heap never had it for 8-bit data anyway (IRAM: MALLOC_CAP_EXEC | 32BIT only).

C: the reserved regions are read by the heap's init, before any C++ constructor; the address of a link symbol is a
constant here

*/

#include <sdkconfig.h>

#include <stdint.h>

#include <heap_memory_layout.h>

// the end of SRAM0 on the instruction bus (ESP-IDF soc/esp32 memory_layout.c: 0x40080000 .. 0x400A0000 IRAM)
#define MODULES_POOL_END 0x400A0000

extern int _iram_end;

#if MODULES_SUPPORT

// _iram_end is word aligned (the IRAM text section of ESP-IDF's link script ends ALIGN(4))
SOC_RESERVE_MEMORY_REGION((intptr_t)&_iram_end, MODULES_POOL_END, espurna_modules_iram);

uint32_t modulesPoolBase(void) {
    return (uint32_t)&_iram_end;
}

uint32_t modulesPoolEnd(void) {
    return MODULES_POOL_END;
}

#endif
