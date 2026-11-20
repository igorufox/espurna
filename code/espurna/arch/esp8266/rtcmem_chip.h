/*

RTCMEM of the ESP8266 on the RTOS SDK (rtcmem.h): RTC_NOINIT memory (the 512 bytes of the RTC user memory,
arch/esp8266/rtcmem_esp8266.cpp), all of it survives a software reset and deep sleep

*/

#pragma once

// The structure itself is all there is (rpn mem_read / mem_write, RTCMEM.DUMP)
#define RTCMEM_ADDR (reinterpret_cast<uintptr_t>(Rtcmem))
#define RTCMEM_BLOCKS (sizeof(RtcmemData) / 4u)

// Rtcmem->sys survives a reset with the rest of the RTCMEM
inline bool rtcmemSysStatus() {
    return rtcmemStatus();
}

// the module guard found the loop stalled (modules.cpp): the stall record of the common watcher, kept while the loop
// stands (arch/rtos/system_rtos.cpp)
void rtcmemStallSnapshot();
