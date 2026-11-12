/*

TLS - the rest of TLS 1.2 (libs/tls.h), for any server, HTTP and MQTT: the one implementation of
- ECDHE (P-256, P-384, P-521, X25519) with RSA and ECDSA certificates
- ChaCha20-Poly1305 next to AES-128-GCM
- the server key by the X.509 decoder of BearSSL (RSA or EC)
- the chain of the server checked against trust anchors (caCheck(): a CA of the settings, or the built-in root)

ESPurna: the tls module (module_tls.cpp) hands these three to the head's TLS through the module API. The ESP32
safeboot (code/safeboot-app) has them always. code/migration-app has not (every KB counts there, it downloads from GitHub
Pages, which takes the base suites).

*/

#pragma once

#include "tls.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#ifndef strlen_P
#define strlen_P strlen
#endif

namespace espurna {
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

    br_x509_decoder_init(decoder, nullptr, nullptr, nullptr, nullptr);
    br_x509_decoder_push(decoder, der, len);
    const auto* pkey = br_x509_decoder_get_pkey(decoder);
    if (!pkey) {
        free(decoder);
        return nullptr;
    }

    *out = *pkey;
    return decoder;
}

// --- the chain of the server checked against trust anchors (MQTT mqttScCheck 2) -----------------------------------
//
// The anchors: the CA certificates of `pem` (PEM, one or more CERTIFICATE blocks: a broker with its own CA); with no
// `pem`, the built-in root. Everything of a connection is one heap block (the X.509 engine, the anchors and their DN
// and key bytes), freed by the TLS after the connection. The current time comes from the clock (NTP): before it is
// set, nothing passes (BR_ERR_X509_TIME_UNKNOWN)

namespace ca {

constexpr size_t AnchorsMax { 4 };
constexpr size_t DataMax { 3072 };     // DN + key bytes of the anchors (an RSA-4096 root: ~650)
constexpr size_t DerMax { 2048 };      // one certificate while it is decoded

// ISRG Root X1, the root of Let's Encrypt (RSA-4096, valid until 2035-06-04; espurna/static/letsencrypt_isrgroot_pem.h)
const char BuiltinRoots[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

struct Block {
    br_x509_minimal_context ctx;
    br_x509_trust_anchor anchors[AnchorsMax];
    size_t count;
    size_t used;
    uint8_t data[DataMax];
};

struct Buffer {
    uint8_t* out;
    size_t size;
    size_t len;
    bool overflow;
};

inline void append(void* ctx, const void* data, size_t len) {
    auto* buffer = static_cast<Buffer*>(ctx);
    if (buffer->len + len > buffer->size) {
        buffer->overflow = true;
        return;
    }

    memcpy(buffer->out + buffer->len, data, len);
    buffer->len += len;
}

inline uint8_t* store(Block& block, const uint8_t* data, size_t len) {
    if (block.used + len > DataMax) {
        return nullptr;
    }

    auto* out = block.data + block.used;
    memcpy(out, data, len);
    block.used += len;
    return out;
}

// one certificate (DER) into a trust anchor: its subject DN and its key
inline bool anchor(Block& block, const uint8_t* der, size_t len) {
    if (block.count >= AnchorsMax) {
        return false;
    }

    auto* decoder = static_cast<br_x509_decoder_context*>(malloc(sizeof(br_x509_decoder_context)));
    if (!decoder) {
        return false;
    }

    Buffer dn { block.data + block.used, DataMax - block.used, 0, false };
    br_x509_decoder_init(decoder, append, &dn, nullptr, nullptr);
    br_x509_decoder_push(decoder, der, len);

    const auto* pkey = br_x509_decoder_get_pkey(decoder);
    bool ok = pkey && !dn.overflow && dn.len && !br_x509_decoder_last_error(decoder);
    if (ok) {
        block.used += dn.len;

        auto& ta = block.anchors[block.count];
        ta.dn.data = dn.out;
        ta.dn.len = dn.len;
        ta.flags = br_x509_decoder_isCA(decoder) ? BR_X509_TA_CA : 0;
        ta.pkey.key_type = pkey->key_type;

        if (pkey->key_type == BR_KEYTYPE_RSA) {
            ta.pkey.key.rsa.n = store(block, pkey->key.rsa.n, pkey->key.rsa.nlen);
            ta.pkey.key.rsa.nlen = pkey->key.rsa.nlen;
            ta.pkey.key.rsa.e = store(block, pkey->key.rsa.e, pkey->key.rsa.elen);
            ta.pkey.key.rsa.elen = pkey->key.rsa.elen;
            ok = ta.pkey.key.rsa.n && ta.pkey.key.rsa.e;
        } else if (pkey->key_type == BR_KEYTYPE_EC) {
            ta.pkey.key.ec.curve = pkey->key.ec.curve;
            ta.pkey.key.ec.q = store(block, pkey->key.ec.q, pkey->key.ec.qlen);
            ta.pkey.key.ec.qlen = pkey->key.ec.qlen;
            ok = ta.pkey.key.ec.q != nullptr;
        } else {
            ok = false;
        }

        if (ok) {
            ++block.count;
        }
    }

    free(decoder);
    return ok;
}

// the CERTIFICATE blocks of a PEM text (in flash: `flash`) into anchors
inline void anchors(Block& block, const char* pem, bool flash) {
    auto* der = static_cast<uint8_t*>(malloc(DerMax));
    if (!der) {
        return;
    }

    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);

    Buffer buffer { der, DerMax, 0, false };
    bool certificate { false };

    const size_t total = flash ? strlen_P(pem) : strlen(pem);
    size_t pos = 0;
    bool error { false };
    while (!error && (pos <= total)) {
        // the text in pieces (a flash string is read with memcpy_P), then a newline that ends the last line
        char chunk[64];
        size_t len;
        if (pos < total) {
            len = std::min(sizeof(chunk), total - pos);
            if (flash) {
                memcpy_P(chunk, pem + pos, len);
            } else {
                memcpy(chunk, pem + pos, len);
            }
        } else {
            chunk[0] = '\n';
            len = 1;
        }
        pos += len;

        size_t offset = 0;
        while (offset < len) {
            offset += br_pem_decoder_push(&pc, chunk + offset, len - offset);
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                certificate = (strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") == 0)
                    || (strcmp(br_pem_decoder_name(&pc), "X509 CERTIFICATE") == 0);
                buffer.len = 0;
                buffer.overflow = false;
                br_pem_decoder_setdest(&pc, certificate ? append : nullptr, &buffer);
                break;
            case BR_PEM_END_OBJ:
                if (certificate && !buffer.overflow) {
                    anchor(block, der, buffer.len);
                }
                certificate = false;
                break;
            case BR_PEM_ERROR:
                error = true;
                offset = len;
                break;
            }
        }
    }

    free(der);
}

// -1 before the validity, +1 after it, 0 within, 2: the clock is not set yet (BearSSL: time unknown)
inline int checkTime(void*, uint32_t nb_days, uint32_t nb_seconds, uint32_t na_days, uint32_t na_seconds) {
    const time_t now = ::time(nullptr);
    if (now < 1577836800) { // 2020-01-01
        return 2;
    }

    // days since January 1st, 0 AD: the Unix epoch is day 719528
    const uint32_t days = static_cast<uint32_t>(now / 86400) + 719528;
    const uint32_t seconds = static_cast<uint32_t>(now % 86400);
    if ((days < nb_days) || ((days == nb_days) && (seconds < nb_seconds))) {
        return -1;
    }
    if ((days > na_days) || ((days == na_days) && (seconds > na_seconds))) {
        return 1;
    }

    return 0;
}

} // namespace ca

// the X.509 engine that checks the chain: the anchors of `pem` (nullptr or "" - the built-in root). The block is the
// TLS's to free() after the connection; nullptr - no memory, or no certificate in `pem`
inline const br_x509_class** caCheck(const char* pem, void** out) {
    auto* block = static_cast<ca::Block*>(malloc(sizeof(ca::Block)));
    if (!block) {
        return nullptr;
    }

    block->count = 0;
    block->used = 0;
    if (pem && *pem) {
        ca::anchors(*block, pem, false);
    } else {
        ca::anchors(*block, ca::BuiltinRoots, true);
    }

    if (!block->count) {
        free(block);
        return nullptr;
    }

    auto* ctx = &block->ctx;
    br_x509_minimal_init(ctx, &br_sha256_vtable, block->anchors, block->count);
    br_x509_minimal_set_hash(ctx, br_sha1_ID, &br_sha1_vtable);
    br_x509_minimal_set_hash(ctx, br_sha224_ID, &br_sha224_vtable);
    br_x509_minimal_set_hash(ctx, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(ctx, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(ctx, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_rsa(ctx, br_rsa_pkcs1_vrfy_get_default());
    br_x509_minimal_set_ecdsa(ctx, br_ec_get_default(), br_ecdsa_vrfy_asn1_get_default());
    br_x509_minimal_set_time_callback(ctx, nullptr, ca::checkTime);

    *out = block;
    return &ctx->vtable;
}

} // namespace full
} // namespace tls
} // namespace espurna
