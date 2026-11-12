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

// --- the head's target of the image receiver ---------------------------------------------------------------------

namespace {

String updateError() {
    PrintString out(64);
    Update.printError(out);
    String result(out.c_str());
    result.trim();
    return result;
}

namespace driver = espurna::ota::driver;

// Update is started for one byte more than a file of a known size: it is never 'finished' by itself, so a download
// with the wrong SHA-256 (the client checks it before end()) is aborted and never booted. The settings stay in their
// sectors while the image is written (EEPROM rotation off: on the 1 MB layout Update writes over the second sector)
class UpdateTarget final : public espurna::image::Target {
public:
    const char* begin(const uint8_t* data, size_t len, size_t size) override {
        if (Update.isRunning()) {
            return "an update is in progress";
        }

        // the chip's word on this file (Beken: the bootloader it is for), before anything is written
        const auto* refused = driver::upload_check(data, len);
        if (refused) {
            _error = FPSTR(refused);
            return _error.c_str();
        }

        if (!driver::verify_header(data, len)) {
            return "not a firmware image for this chip (magic byte, flash config)";
        }

        eepromRotate(false);
        driver::update_prepare();

        // the size of a form part may not be known in advance
        if (!Update.begin(size ? (size + 1) : driver::update_size())) {
            eepromRotate(true);
            _error = String(F("the image does not fit: ")) + updateError();
            return _error.c_str();
        }

        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        if (Update.write(const_cast<uint8_t*>(data), len) != len) {
            _error = String(F("write failed: ")) + updateError();
            return _error.c_str();
        }

        otaProgress(Update.progress());
        return nullptr;
    }

    const char* end(size_t size) override {
        if (!Update.end(true)) {
            eepromRotate(true);
            _error = String(F("image check failed: ")) + updateError();
            return _error.c_str();
        }

        DEBUG_MSG_P(PSTR("[OTA] Success: %7u bytes\n"), size);
        return nullptr;
    }

    void abort() override {
        if (Update.isRunning()) {
            Update.end(false);
        }
        eepromRotate(true);
    }

private:
    String _error;
};

} // namespace

// constructed on the first use: the receiver of ota_fetch.cpp is a static object of another file
espurna::image::Target& otaUpdateTarget() {
    static UpdateTarget target;
    return target;
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
    otaFetchSetup();
#if OTA_ARDUINOOTA_SUPPORT
    otaArduinoSetup();
#endif
}

