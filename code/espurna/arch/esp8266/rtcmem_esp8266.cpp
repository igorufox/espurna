/*

RTCMEM of the ESP8266 (rtcmem.h): the user part of the RTC memory

*/

#include "espurna.h"
#include "rtcmem.h"

volatile RtcmemData* Rtcmem = reinterpret_cast<volatile RtcmemData*>(RTCMEM_ADDR);

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
