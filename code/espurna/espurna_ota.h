/*

OTA MODULE

*/

#pragma once

#include "system.h"
#include "libs/image_receiver.h"

// Main entrypoint for basic OTA methods
// (like clients, arduinoota and basic web)
void otaSetup();

void otaWebSetup();
void otaArduinoSetup();

// The head's target of the one image receiver (libs/image_receiver.h): Arduino Update with the checks of the chip
// (arch/<chip>/ota_driver_<chip>.h). /upgrade and the update by an URL write through it; one image at a time
espurna::image::Target& otaUpdateTarget();

// Helper methods from UpdaterClass that need to be called manually for async mode,
// because we are not using Stream interface to feed it data.
bool otaVerifyHeader(uint8_t* data, size_t len);

void otaProgress(size_t bytes, size_t each);
void otaProgress(size_t bytes);

#if OTA_WEB_SUPPORT
// An upload to /upgrade is being received, or went in and the board is about to restart into it
bool otaWebUploading();

// systemHostname() inside a "..." string of a page script
inline String otaWebJsString(const String& value) {
    String out;
    out.reserve(value.length() + 8);

    for (size_t index = 0; index < value.length(); ++index) {
        const auto c = value[index];
        if ((c == '"') || (c == '\\') || (c == '<') || (c == '>') || (c == '&') || (c == '\'')
            || (static_cast<unsigned char>(c) < 0x20))
        {
            char escaped[8];
            snprintf_P(escaped, sizeof(escaped), PSTR("\\u%04x"), static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += escaped;
            continue;
        }

        out += c;
    }

    return out;
}
#endif

void otaPrintError();
bool otaFinalize(size_t size, CustomResetReason reason, bool evenIfRemaining);
bool otaFinalize(size_t size, CustomResetReason reason);

