/*

HASH - SHA-256, SHA-1 and MD5 of everything ESPurna builds: the check of a download (http_client_impl.h), the
WebSocket accept key (websocket_impl.h), the digest login (http_server_impl.h).

One crypto (owner 2026-11-18: mbedtls on every chip), one version (owner 2026-11-17): mbedTLS 3.6.6 of
code/libraries.ini on the three chips (ESP-IDF 5.5 on the ESP32, the package framework-mbedtls in the ESP8266 SDK and
the Beken BDK); MD5 of the ESP8266: its ROM.

    espurna::hash::Sha256 sha;
    sha.update(data, size);
    uint8_t digest[espurna::hash::Sha256::Size];
    sha.finish(digest);

*/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

// The mbedtls of this build is the version of code/libraries.ini (passed by every config as -DESPURNA_MBEDTLS): the
// build stops when a chip's SDK brought another one
#ifndef ESPURNA_MBEDTLS
#error "ESPURNA_MBEDTLS is not set: the config must pass -DESPURNA_MBEDTLS=\"${libraries.mbedtls}\" (code/libraries.ini)"
#endif

namespace espurna {
namespace hash {
namespace mbedtls_version {

constexpr bool same(const char* lhs, const char* rhs) {
    return (*lhs == *rhs) && ((*lhs == '\0') || same(lhs + 1, rhs + 1));
}

static_assert(same(MBEDTLS_VERSION_STRING, ESPURNA_MBEDTLS),
    "the mbedtls of this chip is not the version of code/libraries.ini (mbedtls)");

} // namespace mbedtls_version
} // namespace hash
} // namespace espurna

// ESP8266: MD5 is in the ROM of the chip (esp8266.rom.ld of the SDK package), nothing of mbedtls for them
#if defined(ESP8266)
extern "C" {

struct espurna_rom_md5_ctx {
    uint32_t buf[4];
    uint32_t bits[2];
    uint8_t in[64];
};

void MD5Init(espurna_rom_md5_ctx*);
void MD5Update(espurna_rom_md5_ctx*, const unsigned char*, unsigned int);
void MD5Final(unsigned char[16], espurna_rom_md5_ctx*);


} // extern "C"

namespace espurna {
namespace hash {
namespace rom {

// The ROM's MD5 reads its input by bytes: data in flash (the .rodata of a module, PSTR) has no byte loads on the
// ESP8266. Flash data goes through a buffer on the stack, read by aligned words
template <typename Update, typename Context>
void update(Update call, Context* ctx, const void* data, size_t size) {
    const auto address = reinterpret_cast<uintptr_t>(data);
    if ((address < 0x40200000) || (address >= 0x40300000)) {
        call(ctx, static_cast<const unsigned char*>(data), size);
        return;
    }

    uint32_t words[16];
    uintptr_t at = address;
    while (size) {
        const uintptr_t base = at & ~uintptr_t{3};
        const size_t skip = at - base;
        const size_t chunk = std::min(size, sizeof(words) - skip);
        const size_t count = (skip + chunk + 3) / 4;
        for (size_t index = 0; index < count; ++index) {
            words[index] = reinterpret_cast<const volatile uint32_t*>(base)[index];
        }
        call(ctx, reinterpret_cast<const unsigned char*>(words) + skip, chunk);
        at += chunk;
        size -= chunk;
    }
}

} // namespace rom
} // namespace hash
} // namespace espurna
#endif

namespace espurna {
namespace hash {

class Sha256 {
public:
    static constexpr size_t Size { 32 };

    Sha256() {
        mbedtls_sha256_init(&_ctx);
        mbedtls_sha256_starts(&_ctx, 0);
    }

    ~Sha256() {
        mbedtls_sha256_free(&_ctx);
    }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, size_t size) {
        mbedtls_sha256_update(&_ctx, static_cast<const unsigned char*>(data), size);
    }

    void finish(uint8_t (&out)[Size]) {
        mbedtls_sha256_finish(&_ctx, out);
    }

private:
    mbedtls_sha256_context _ctx;
};

// SHA-1 of mbedtls on every chip: the ROM's SHA1Update / SHA1Final of the ESP8266 gave a wrong digest (the WebSocket
// accept key differed on every handshake but the first after a boot, 18.11; its work memory is not ours to know)
class Sha1 {
public:
    static constexpr size_t Size { 20 };

    Sha1() {
        mbedtls_sha1_init(&_ctx);
        mbedtls_sha1_starts(&_ctx);
    }

    ~Sha1() {
        mbedtls_sha1_free(&_ctx);
    }

    Sha1(const Sha1&) = delete;
    Sha1& operator=(const Sha1&) = delete;

    void update(const void* data, size_t size) {
        mbedtls_sha1_update(&_ctx, static_cast<const unsigned char*>(data), size);
    }

    void finish(uint8_t (&out)[Size]) {
        mbedtls_sha1_finish(&_ctx, out);
    }

private:
    mbedtls_sha1_context _ctx;
};

#if defined(ESP8266)
class Md5 {
public:
    static constexpr size_t Size { 16 };

    Md5() {
        MD5Init(&_ctx);
    }

    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    void update(const void* data, size_t size) {
        rom::update(MD5Update, &_ctx, data, size);
    }

    void finish(uint8_t (&out)[Size]) {
        MD5Final(out, &_ctx);
    }

private:
    espurna_rom_md5_ctx _ctx;
};
#else
class Md5 {
public:
    static constexpr size_t Size { 16 };

    Md5() {
        mbedtls_md5_init(&_ctx);
        mbedtls_md5_starts(&_ctx);
    }

    ~Md5() {
        mbedtls_md5_free(&_ctx);
    }

    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    void update(const void* data, size_t size) {
        mbedtls_md5_update(&_ctx, static_cast<const unsigned char*>(data), size);
    }

    void finish(uint8_t (&out)[Size]) {
        mbedtls_md5_finish(&_ctx, out);
    }

private:
    mbedtls_md5_context _ctx;
};

#endif

} // namespace hash
} // namespace espurna

