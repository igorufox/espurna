/*

RTCMEM of the ESP8266 (rtcmem.h): the user part of the RTC memory at a fixed address, all of it survives a software
reset

*/

#pragma once

// Base address of USER RTC memory
// https://github.com/esp8266/esp8266-wiki/wiki/Memory-Map#memmory-mapped-io-registers
#define RTCMEM_ADDR_BASE (0x60001200)

// RTC memory is accessed using blocks of 4 bytes.
// Blocks 0..63 are reserved by the SDK, 64..192 are available to the user.
// Blocks 64..96 are reserved by the eboot 'struct eboot_command' (128 -> (128 / 4) -> 32):
// https://github.com/esp8266/Arduino/blob/master/bootloaders/eboot/eboot_command.h
#define RTCMEM_OFFSET 32u
#define RTCMEM_ADDR (RTCMEM_ADDR_BASE + (RTCMEM_OFFSET * 4u))

#define RTCMEM_BLOCKS 96u

// Rtcmem->sys survives a reset with the rest of the RTCMEM
inline bool rtcmemSysStatus() {
    return rtcmemStatus();
}
