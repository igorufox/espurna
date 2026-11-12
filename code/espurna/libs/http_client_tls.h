/*

HTTP CLIENT - the rest of TLS 1.2 (libs/http_client.h), for any server: the one implementation of
- ECDHE (P-256, P-384, P-521, X25519) with RSA and ECDSA certificates
- ChaCha20-Poly1305 next to AES-128-GCM
- the server key by the X.509 decoder of BearSSL (RSA or EC). The chain is still not validated (no CA store)

ESPurna: the tls module (module_tls.cpp) hands these two to the head's client through the module API. The ESP32
safeboot (code/safeboot-app) has them always. code/mini-app has not (every KB counts there, it downloads from GitHub
Pages, which takes the base suites).

*/

#pragma once

#include "http_client.h"

#include <cstdlib>
#include <cstring>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif

namespace espurna {
namespace http {
namespace tls {
namespace full {

// the base suite (static RSA) last: offered to the servers that take nothing else
const uint16_t Suites[] PROGMEM {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
};

// after the base setup: the suites and what they need (the base set SHA-256, its PRF, AES-GCM, the RSA encryption)
inline bool extend(br_ssl_client_context* sc) {
    auto* eng = &sc->eng;

    uint16_t suites[sizeof(Suites) / sizeof(Suites[0])];
    memcpy_P(suites, Suites, sizeof(Suites));
    br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));

    br_ssl_engine_set_default_rsavrfy(eng);
    br_ssl_engine_set_ec(eng, &br_ec_all_m15);
    br_ssl_engine_set_default_ecdsa(eng);

    br_ssl_engine_set_chapol(eng, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
    br_ssl_engine_set_chacha20(eng, &br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(eng, &br_poly1305_ctmul_run);

    return true;
}

// the decoder keeps the key's bytes: the client frees the block after the connection
inline void* key(const uint8_t* der, size_t len, br_x509_pkey* out) {
    auto* decoder = static_cast<br_x509_decoder_context*>(malloc(sizeof(br_x509_decoder_context)));
    if (!decoder) {
        return nullptr;
    }

    br_x509_decoder_init(decoder, nullptr, nullptr);
    br_x509_decoder_push(decoder, der, len);
    const auto* pkey = br_x509_decoder_get_pkey(decoder);
    if (!pkey) {
        free(decoder);
        return nullptr;
    }

    *out = *pkey;
    return decoder;
}

} // namespace full
} // namespace tls
} // namespace http
} // namespace espurna
