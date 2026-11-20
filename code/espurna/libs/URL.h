// -----------------------------------------------------------------------------
// An URL as Arduino Strings, parsed by the one URL parser (libs/url_parse.h)
// -----------------------------------------------------------------------------

#pragma once


#include <cstdint>
#include <utility>

#include "../types.h"
#include "url_parse.h"

class URL {
public:
    URL() = default;
    URL(const URL&) = default;
    URL(URL&&) = default;

    URL& operator=(const URL&) = default;
    URL& operator=(URL&&) = default;

    explicit URL(espurna::StringView string) {
        espurna::url::Parts parts;
        if (espurna::url::parse(string.data(), string.length(), parts)) {
            protocol = parts.scheme;
            host = parts.host;
            path = parts.path;
            port = parts.port;
        }
    }

    espurna::string protocol;
    espurna::string host;
    espurna::string path;
    uint16_t port { 0 };
};
