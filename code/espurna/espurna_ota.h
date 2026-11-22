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

// The head's target of the one image receiver (libs/image_receiver.h): the image into the chip (ota_driver.h,
// arch/<chip>/ota_<chip>.cpp). /upgrade and espota write through it; one image at a time
espurna::image::Target& otaUpdateTarget();

void otaProgress(size_t bytes, size_t each);
void otaProgress(size_t bytes);

#if OTA_WEB_SUPPORT
// An upload to /upgrade is being received, or went in and the board is about to restart into it
bool otaWebUploading();

// systemHostname() inside a "..." string of a page script
inline espurna::string otaWebJsString(const espurna::string& value) {
    espurna::string out;
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

// an image is being written into the update slot
bool otaRunning();
void otaPrintError();

// the espota upload in RtcmemData::ota_trace: running, or stopped by the stall watcher (arch/esp8266/ota_esp8266.cpp).
// Seen at the next boot, the setting otaTrace tells what it took
namespace espurna {
namespace ota {

constexpr uint32_t TraceRunning { 0x4F544131 };
constexpr uint32_t TraceStalled { 0x4F544132 };

} // namespace ota
} // namespace espurna

