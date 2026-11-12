/*

OTA MODULE

*/

#pragma once

#include "system.h"

// Main entrypoint for basic OTA methods
// (like clients, arduinoota and basic web)
void otaSetup();

void otaWebSetup();
void otaArduinoSetup();

// Helper methods from UpdaterClass that need to be called manually for async mode,
// because we are not using Stream interface to feed it data.
bool otaVerifyHeader(uint8_t* data, size_t len);

void otaProgress(size_t bytes, size_t each);
void otaProgress(size_t bytes);

// Update from an URL, every chip (ota_fetch.cpp): the WebUI (/ota/fetch), `ota <url>`, MQTT ota. The next boot
// (ESP8266, Beken) or safeboot (ESP32) downloads it
void otaFetchSetup();
bool otaFetchRequest(const String& url, const String& sha256, String& error);
String otaFetchStatus();
String otaFetchResult();

#if OTA_WEB_SUPPORT
// An upload to /upgrade is being received, or went in and the board is about to restart into it
bool otaWebUploading();

// The POSTs that change the firmware (/upgrade, /ota/fetch) of both web servers: a browser sends Origin with them, a
// page of another site must not make the board flash something (the browser keeps the credentials of the board). No
// Origin at all is a client that is not a browser (curl, the release scripts), it passes; "null" does not. Behind a
// reverse proxy the Host the board sees is its own address: the proxy has to pass the one of the browser, as Host or
// as X-Forwarded-Host
inline bool otaWebSameOrigin(const String& origin, const String& host, const String& forwarded_host) {
    if (!origin.length()) {
        return true;
    }

    const int scheme = origin.indexOf("://");
    if (scheme < 0) {
        return false;
    }

    const auto from = origin.substring(static_cast<unsigned int>(scheme + 3));
    return (host.length() && from.equalsIgnoreCase(host))
        || (forwarded_host.length() && from.equalsIgnoreCase(forwarded_host));
}

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

