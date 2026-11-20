/*

RTCMEM of the ESP8266 on the RTOS SDK (rtcmem.h): RTC_NOINIT memory

*/

#include "espurna.h"
#include "rtcmem.h"

// RTC_NOINIT_ATTR keeps the values across SW / WDT / panic resets and deep sleep
RTC_NOINIT_ATTR static RtcmemData _rtcmem_storage;
volatile RtcmemData* Rtcmem = &_rtcmem_storage;

// Treat memory as dirty on cold boot, hardware wdt reset and rst pin
bool rtcmemChipKept() {
    switch (systemResetReason()) {
    case REASON_EXT_SYS_RST:
    case REASON_DEFAULT_RST:
        return false;
    default:
        break;
    }

    return true;
}

// nothing more of the chip
void rtcmemChipSetup() {
}
