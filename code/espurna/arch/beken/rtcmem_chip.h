/*

RTCMEM of the Beken (rtcmem.h): plain RAM (the bootloader clears all of it, arch/beken/rtcmem_beken.cpp); only the system
word survives, in the retention register

*/

#pragma once

// The structure itself is all there is (rpn mem_read / mem_write, RTCMEM.DUMP)
#define RTCMEM_ADDR (reinterpret_cast<uintptr_t>(Rtcmem))
#define RTCMEM_BLOCKS (sizeof(RtcmemData) / 4u)

// Only Rtcmem->sys survives a reset here (ref. rtcmem_beken.cpp)
bool rtcmemSysStatus();
uint32_t rtcmemRetention();

// Exception that caused the last reset, written into the register by our crash handlers
enum class RtcmemCrash : uint32_t {
    None = 0,
    Undefined,
    PrefetchAbort,
    DataAbort,
    Reserved,
    // Only in the flash record (written by the malloc failed hook), never in the register
    MallocFailed,
    // Only in the flash record: the watchdog was about to reset the chip, written by the stall watcher with the
    // saved context of every task
    Stall,
};

RtcmemCrash rtcmemCrash();

// Registers and stack of the last crash, kept in the flash (ref. rtcmem_beken.cpp). Setup reports a new one once,
// in the log and on MQTT, dump prints the whole record (CRASH command)
void rtcmemCrashSetup();
void rtcmemCrashDump(Print&);
