/*

OTA MODULE COMMON FUNCTIONS

*/

#include "espurna.h"
#include "espurna_ota.h"
#include "system.h"
#include "terminal.h"
#include "utils.h"

#if WEB_SUPPORT
#include "ws.h"
#endif

#include "libs/PrintString.h"

#include "ota_driver.h"

void otaPrintError() {
#if DEBUG_SUPPORT
    if (Update.hasError()) {
        PrintString out(64);
        Update.printError(out);
        DEBUG_MSG_P(PSTR("[OTA] %s\n"), out.c_str());
    }
#endif
}

bool otaFinalize(size_t size, CustomResetReason reason, bool evenIfRemaining) {
    if (Update.isRunning() && Update.end(evenIfRemaining)) {
        DEBUG_MSG_P(PSTR("[OTA] Success: %7u bytes\n"), size);
        prepareReset(reason);
        return true;
    }

    otaPrintError();
    eepromRotate(true);

    return false;
}

bool otaFinalize(size_t size, CustomResetReason reason) {
    return otaFinalize(size, reason, false);
}

// Helper methods from UpdaterClass that need to be called manually for async mode,
// because we are not using Stream interface to feed it data.
bool otaVerifyHeader(uint8_t* data, size_t len) {
    return espurna::ota::driver::verify_header(data, len);
}

void otaProgress(size_t bytes, size_t each) {
    // Removed to avoid websocket ping back during upgrade (see #1574)
    // TODO: implement as separate from debugging message
    #if WEB_SUPPORT
        if (wsConnected()) return;
    #endif

    // Telnet and serial will still output things, but slightly throttled
    static size_t last = 0;
    if (bytes < last) {
        last = 0;
    }

    if ((bytes > each) && (bytes - each > last)) {
        DEBUG_MSG_P(PSTR("[OTA] Progress: %7u bytes\r"), bytes);
        last = bytes;
    }
}

void otaProgress(size_t bytes) {
    constexpr size_t Each { 8192 };
    otaProgress(bytes, Each);
}

void otaSetup() {
#if defined(ARDUINO_ARCH_ESP8266) && OTA_WEB_SUPPORT
    otaFetchSetup();
#endif
#if OTA_ARDUINOOTA_SUPPORT
    otaArduinoSetup();
#endif
#if !WEB_SUPPORT && OTA_WEB_SUPPORT
    otaWebSetup();
#endif
#if OTA_CLIENT != OTA_CLIENT_NONE
    otaClientSetup();
#endif
}

