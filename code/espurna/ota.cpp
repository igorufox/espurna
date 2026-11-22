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

// The image into the chip (ota_driver.h): one upload at a time. The settings stay in their sectors while the image is
// written (EEPROM rotation off: on the 1 MB layout the image goes over the second sector)
namespace {

namespace driver = espurna::ota::driver;

struct Writer {
    bool running { false };
    size_t progress { 0 };
    espurna::string error;
};

Writer writer;

const char* failed(const char* error) {
    writer.running = false;
    writer.error = error;
    eepromRotate(true);
    return writer.error.c_str();
}

class UpdateTarget final : public espurna::image::Target {
public:
    const char* begin(const uint8_t* data, size_t len, size_t size) override {
        if (writer.running) {
            return "an update is in progress";
        }

        eepromRotate(false);
        writer.error = espurna::string();
        writer.progress = 0;

        const auto* error = driver::begin(data, len, size);
        if (error) {
            return failed(error);
        }

        writer.running = true;
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        const auto* error = driver::write(data, len);
        if (error) {
            driver::abort();
            return failed(error);
        }

        writer.progress += len;
        otaProgress(writer.progress);
        return nullptr;
    }

    const char* end(size_t size) override {
        const auto* error = driver::end();
        if (error) {
            return failed(error);
        }

        writer.running = false;
        DEBUG_MSG_P(PSTR("[OTA] Success: %7u bytes\n"), size);
        return nullptr;
    }

    void abort() override {
        if (writer.running) {
            driver::abort();
        }

        writer.running = false;
        eepromRotate(true);
    }
};

} // namespace

bool otaRunning() {
    return writer.running;
}

void otaPrintError() {
#if DEBUG_SUPPORT
    if (writer.error.length()) {
        DEBUG_MSG_P(PSTR("[OTA] %s\n"), writer.error.c_str());
    }
#endif
}

// constructed on the first use: the receivers of the uploads are static objects of other files
espurna::image::Target& otaUpdateTarget() {
    static UpdateTarget target;
    return target;
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
    espurna::ota::driver::confirm();
#if OTA_ARDUINOOTA_SUPPORT
    otaArduinoSetup();
#endif
}

