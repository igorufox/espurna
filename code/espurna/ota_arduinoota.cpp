/*

ESPOTA MODULE: the espota receiver of every chip (libs/espota.h), espota.py and PlatformIO's upload_protocol = espota

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#if OTA_ARDUINOOTA_SUPPORT

#include "espurna_ota.h"
#include "system.h"

#if MODULES_SUPPORT
#include "modules.h"
#endif
#include "ws.h"

#include "libs/espota.h"
#include "ota_driver.h"
#include "rtcmem.h"

namespace espurna {
namespace ota {
namespace arduino {
namespace {

// the image goes where /upgrade writes it (otaUpdateTarget()); the mdns module announces the service
espota::Server server;

uint32_t nonce_seed() {
    return randomNumber();
}

void configure() {
#if USE_PASSWORD
    server.password(systemPassword().c_str());
#else
    server.password(nullptr);
#endif
    server.begin(OTA_PORT, otaUpdateTarget(), nonce_seed);
}

void loop() {
    server.handle();
}

void start() {
#if MODULES_SUPPORT
    // the image is written where the modules are: they do not start again
    modulesForget();
#endif

    // Disabling EEPROM rotation to prevent writing to EEPROM after the upgrade
    // espota takes the whole image inside one loop(): force the backup right now instead of waiting for the next loop()
    eepromRotate(false);
    eepromBackup(0);
    Rtcmem->ota_trace = TraceRunning;
    Rtcmem->ota_bytes = 0;
    Rtcmem->ota_size = 0;
    driver::espota_started();

    DEBUG_MSG_P(PSTR("[OTA] Started...\n"));
#if WEB_SUPPORT
    wsSend([](espurna::json::Writer& root) {
        root.field("message", "OTA update started.");
    });
#endif
}

void end() {
    // the restart comes from the loop, as after /upgrade. Page reload will happen automatically, when WebUI will fail
    // to receive the PING response.
    driver::espota_stopped();
    Rtcmem->ota_trace = 0;
    DEBUG_MSG_P(PSTR("[OTA] Done, restarting.\n"));
    prepareReset(CustomResetReason::Ota);
}

// between the waits of the transfer: the watchdogs, and the other tasks get the CPU
void idle() {
    driver::espota_progress();
    espurna::time::blockingDelay(
        espurna::duration::Milliseconds(1));
}

void progress(size_t progress, size_t total) {
    // espota receives the whole image inside of a single loop() call
    Rtcmem->ota_bytes = progress;
    Rtcmem->ota_size = total;
    driver::espota_progress();

    // Removed to avoid websocket ping back during upgrade (see #1574)
    // TODO: implement as a custom payload that reports progress in non-text form?
#if WEB_SUPPORT
    if (wsConnected()) {
        return;
    }
#endif

#if DEBUG_SUPPORT
    static size_t last;
    const size_t current = progress / ((total >= 100) ? (total / 100) : 1);
    if (current != last) {
        DEBUG_MSG_P(PSTR("[OTA] Progress: %u%%\r"), static_cast<unsigned>(current));
        last = current;
    }
#endif
}

void error(espota::Error error, const char* text) {
    driver::espota_stopped();
    Rtcmem->ota_trace = 0;

#if DEBUG_SUPPORT
    StringView reason = STRING_VIEW("Unknown");

    switch (error) {
    case espota::Error::Auth:
        reason = STRING_VIEW("Authentication");
        break;
    case espota::Error::Begin:
        reason = STRING_VIEW("Begin");
        break;
    case espota::Error::Connect:
        reason = STRING_VIEW("Connection");
        break;
    case espota::Error::Receive:
        reason = STRING_VIEW("Receive");
        break;
    case espota::Error::End:
        reason = STRING_VIEW("End");
        break;
    }

    DEBUG_MSG_P(PSTR("[OTA] OTA Stopped: %.*s, %s\n"),
        reason.length(), reason.data(), text ? text : "");
    otaPrintError();
#else
    (void)error;
    (void)text;
#endif

    eepromRotate(true);
}

// an upload this board restarted in the middle of: what it took, for `get otaTrace` (W58)
void trace() {
    if (!rtcmemStatus()) {
        return;
    }

    const auto tag = Rtcmem->ota_trace;
    if ((tag == TraceRunning) || (tag == TraceStalled)) {
        char out[96];
        snprintf_P(out, sizeof(out), PSTR("%s at %u of %u bytes"),
            (tag == TraceStalled) ? "stalled, restarted" : "restarted",
            Rtcmem->ota_bytes, Rtcmem->ota_size);
        setSetting("otaTrace", out);
        DEBUG_MSG_P(PSTR("[OTA] The last espota upload %s\n"), out);
    }

    Rtcmem->ota_trace = 0;
}

void setup() {
    trace();
    espurnaRegisterLoop(loop);
    espurnaRegisterReload(configure);

    server.onStart(start);
    server.onEnd(end);
    server.onError(error);
    server.onProgress(progress);
    server.onIdle(idle);

    configure();
}

} // namespace
} // namespace arduino
} // namespace ota
} // namespace espurna

void otaArduinoSetup() {
    espurna::ota::arduino::setup();
}

#endif // OTA_ARDUINOOTA_SUPPORT
