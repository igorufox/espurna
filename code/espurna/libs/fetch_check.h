/*

The checks of an image download request (the URL and the SHA-256 of the file), one version for every image that
takes one: ESPurna (ota_fetch.cpp, arch/esp32/safeboot_esp32.cpp), code/mini-app and code/safeboot-app.
Plain C++, no framework: any string type with c_str() and length() (Arduino String, std::string).

*/

#pragma once

#include <cstddef>
#include <cstring>

namespace espurna {
namespace fetch_check {

// goes into the settings, the request line and the JSON status as is
constexpr size_t UrlMax { 240 };

inline bool urlScheme(const char* url, size_t length) {
    return ((length >= 7) && !std::strncmp(url, "http://", 7))
        || ((length >= 8) && !std::strncmp(url, "https://", 8));
}

// http:// or https://, 12..240 characters, printable ASCII without space, quotes, backslash, < and >
inline bool validUrl(const char* url, size_t length) {
    if ((length < 12) || (length > UrlMax) || !urlScheme(url, length)) {
        return false;
    }

    for (size_t index = 0; index < length; ++index) {
        const char c = url[index];
        if ((c <= ' ') || (c == '"') || (c == '\\') || (c == '<') || (c == '>') || (c > '~')) {
            return false;
        }
    }

    return true;
}

// 64 lower case hex digits
inline bool validSha256(const char* value, size_t length) {
    if (length != 64) {
        return false;
    }

    for (size_t index = 0; index < length; ++index) {
        const char c = value[index];
        if (!(((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')))) {
            return false;
        }
    }

    return true;
}

template <typename T>
inline bool validUrl(const T& url) {
    return validUrl(url.c_str(), url.length());
}

template <typename T>
inline bool validSha256(const T& value) {
    return validSha256(value.c_str(), value.length());
}

} // namespace fetch_check
} // namespace espurna
