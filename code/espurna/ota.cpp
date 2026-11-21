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

#include "ota_driver.h"

// The image into the chip's update slot (chip::ota_* of arch/rtos/chip_sdk.h): begin() erases what it needs, write()
// takes it in any parts, end() checks it and makes it the next boot. No Update of the layer
namespace {

struct Writer {
    bool running { false };
    size_t progress { 0 };
    const char* error { nullptr };
};

Writer writer;

bool writerBegin(size_t size) {
    writer.error = espurna::core::chip::ota_begin(size);
    writer.running = (writer.error == nullptr);
    writer.progress = 0;
    return writer.running;
}

bool writerWrite(const uint8_t* data, size_t len) {
    writer.error = espurna::core::chip::ota_write(data, len);
    if (writer.error) {
        return false;
    }

    writer.progress += len;
    return true;
}

bool writerEnd() {
    writer.running = false;
    writer.error = espurna::core::chip::ota_end();
    return writer.error == nullptr;
}

void writerAbort() {
    if (writer.running) {
        espurna::core::chip::ota_abort();
    }
    writer.running = false;
}

} // namespace

bool otaRunning() {
    return writer.running;
}

void otaPrintError() {
#if DEBUG_SUPPORT
    if (writer.error) {
        DEBUG_MSG_P(PSTR("[OTA] %s\n"), writer.error);
    }
#endif
}

bool otaFinalize(size_t size, CustomResetReason reason, bool) {
    if (writer.running && writerEnd()) {
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

espurna::string updateError() {
    return espurna::string(writer.error ? writer.error : "");
}

namespace driver = espurna::ota::driver;

// Update is started for the exact size of the file when it is known (an upload of a raw body), else for what the chip
// takes. Not one byte more: LibreTiny's UF2 writer refuses the first block when the size is not exactly its block
// count x 512 (the Beken took no raw upload, W30; the +1 kept a download with a wrong SHA-256 from finishing, and the
// head downloads nothing any more). The settings stay in their sectors while the image is written (EEPROM rotation
// off: on the 1 MB layout Update writes over the second sector)
class UpdateTarget final : public espurna::image::Target {
public:
    const char* begin(const uint8_t* data, size_t len, size_t size) override {
        if (writer.running || _own) {
            return "an update is in progress";
        }

        // a file the chip writes itself (Beken: the RBL head of the install image)
        if (driver::own_image(data, len)) {
            const auto* refused = driver::own_begin(data, len);
            if (refused) {
                _error = refused;
                return _error.c_str();
            }
            eepromRotate(false);
            _own = true;
            return nullptr;
        }

        // the chip's word on this file (Beken: the bootloader it is for), before anything is written
        const auto* refused = driver::upload_check(data, len);
        if (refused) {
            _error = refused;
            return _error.c_str();
        }

        if (!driver::verify_header(data, len)) {
            return "not a firmware image for this chip (magic byte, flash config)";
        }

        eepromRotate(false);
        driver::update_prepare();

        // the size of a form part may not be known in advance
        if (!writerBegin(size ? size : driver::update_size())) {
            eepromRotate(true);
            _error = espurna::string("the image does not fit: ") + updateError();
            return _error.c_str();
        }

        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        if (_own) {
            const auto* error = driver::own_write(data, len);
            if (error) {
                _error = error;
                return _error.c_str();
            }
            return nullptr;
        }

        if (!writerWrite(data, len)) {
            _error = espurna::string("write failed: ") + updateError();
            return _error.c_str();
        }

        otaProgress(writer.progress);
        return nullptr;
    }

    const char* end(size_t size) override {
        if (_own) {
            _own = false;
            const auto* error = driver::own_end();
            if (error) {
                eepromRotate(true);
                _error = error;
                return _error.c_str();
            }
            DEBUG_MSG_P(PSTR("[OTA] Success: %7u bytes\n"), size);
            return nullptr;
        }

        if (!writerEnd()) {
            eepromRotate(true);
            _error = espurna::string("image check failed: ") + updateError();
            return _error.c_str();
        }

        DEBUG_MSG_P(PSTR("[OTA] Success: %7u bytes\n"), size);
        return nullptr;
    }

    void abort() override {
        if (_own) {
            _own = false;
            driver::own_abort();
        }

        writerAbort();
        eepromRotate(true);
    }

private:
    espurna::string _error;
    bool _own { false };
};

} // namespace

// constructed on the first use: the receivers of the uploads are static objects of other files
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

#if defined(ESP8266)
// a new head confirms itself to the bootloader (arch/esp8266/ota_esp8266.cpp)
void otaConfirmSetup();
#endif

void otaSetup() {
#if defined(ESP8266)
    otaConfirmSetup();
#endif
#if OTA_ARDUINOOTA_SUPPORT
    otaArduinoSetup();
#endif
}

