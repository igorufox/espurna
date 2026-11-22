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
#include "storage_eeprom.h"
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

// espota receives the whole image inside of a single loop() call: the loop watchdog and the loop stall watcher
// (arch/rtos/system_rtos.cpp, SYSTEM_LOOP_STALL s without a loop() pass) are fed between the chunks, a slow upload is
// not aborted
void feed() {
    ::espurna::system::chip::sdk::feed_loop_wdt();
    ::espurna::system::stall::pass();
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
    // pending settings reach the flash before the image goes over the place of their backup
    eepromForceCommit();

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
    Rtcmem->ota_trace = 0;
    DEBUG_MSG_P(PSTR("[OTA] Done, restarting.\n"));
    prepareReset(CustomResetReason::Ota);
}

// between the waits of the transfer: the watchdogs, and the other tasks get the CPU
void idle() {
    feed();
    espurna::time::blockingDelay(
        espurna::duration::Milliseconds(1));
}

void progress(size_t progress, size_t total) {
    // espota receives the whole image inside of a single loop() call
    Rtcmem->ota_bytes = progress;
    Rtcmem->ota_size = total;
    feed();

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
