/*

URL - the one URL parser of everything ESPurna builds (the HTTP client, the terminal commands, code/migration-app,
code/safeboot-app). No Arduino.

    <scheme>://<host>[:<port>][/<path>]   the port defaults to 80 for http, 443 for https, 0 otherwise; the path to "/"

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espurna {
namespace url {

constexpr size_t SchemeMax { 16 };
constexpr size_t HostMax { 128 };
constexpr size_t PathMax { 256 };

struct Parts {
    char scheme[SchemeMax] {};
    char host[HostMax] {};
    uint16_t port { 0 };
    char path[PathMax] {};

    bool is(const char* name) const {
        return strcmp(scheme, name) == 0;
    }
};

// false - no scheme, no host, a part too long, or a port outside 1..65535
inline bool parse(const char* text, size_t len, Parts& out) {
    out = Parts{};

    const char* end = text + len;
    const char* sep = nullptr;
    for (const char* ptr = text; ptr + 2 < end; ++ptr) {
        if ((ptr[0] == ':') && (ptr[1] == '/') && (ptr[2] == '/')) {
            sep = ptr;
            break;
        }
    }

    if (!sep || (sep == text) || (static_cast<size_t>(sep - text) >= SchemeMax)) {
        return false;
    }

    memcpy(out.scheme, text, sep - text);
    out.port = out.is("https") ? 443 : out.is("http") ? 80 : 0;

    const char* host = sep + 3;
    const char* slash = host;
    while ((slash < end) && (*slash != '/')) {
        ++slash;
    }

    const char* colon = host;
    while ((colon < slash) && (*colon != ':')) {
        ++colon;
    }

    const size_t host_len = colon - host;
    if (!host_len || (host_len >= HostMax)) {
        return false;
    }
    memcpy(out.host, host, host_len);

    if (colon < slash) {
        long port { 0 };
        for (const char* ptr = colon + 1; ptr < slash; ++ptr) {
            if ((*ptr < '0') || (*ptr > '9') || (port > 65535)) {
                return false;
            }
            port = (port * 10) + (*ptr - '0');
        }
        if ((port <= 0) || (port > 65535)) {
            return false;
        }
        out.port = static_cast<uint16_t>(port);
    }

    const size_t path_len = end - slash;
    if (path_len >= PathMax) {
        return false;
    }
    if (path_len) {
        memcpy(out.path, slash, path_len);
    } else {
        out.path[0] = '/';
    }

    return true;
}

inline bool parse(const char* text, Parts& out) {
    return parse(text, text ? strlen(text) : 0, out);
}

} // namespace url
} // namespace espurna
