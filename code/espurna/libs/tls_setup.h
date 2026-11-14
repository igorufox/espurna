/*

TLS - the setup of a TLS connection (libs/tls_engine.h start()): the client, the base suite (static RSA +
AES-128-GCM), the server's key without the chain (its fingerprint when asked for), or the chain check. Not in ESPurna's
head: the tls module (module_tls.cpp) has it with the rest of TLS 1.2 (libs/tls_full.h); code/migration-app has the
base alone (GitHub Pages takes it, every KB counts there).

The image that includes this file defines the three extensions (libs/tls_full.h has them; code/migration-app none):
    extend()    after the base setup: more suites and their algorithms. false - none
    key()       the server key from its certificate (DER) by the X.509 decoder, `out` points into the returned block
                (free() it after the connection). nullptr - none, the RSA key is read here
    caCheck()   the X.509 engine that checks the chain against the CA certificates of `pem` (PEM; nullptr or empty -
                the built-in root), it lives in the returned block. nullptr - no check here

*/

#pragma once

#include "tls_engine.h"

#include <cstring>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif

namespace espurna {
namespace tls {

bool extend(br_ssl_client_context* sc);
void* key(const uint8_t* der, size_t len, br_x509_pkey* out);
const br_x509_class** caCheck(const char* pem, void** block);

namespace setup {
namespace {

// --- the server key, without validating the chain; the fingerprint when asked for ---------------------------------

// the key: the X.509 decoder (RSA or EC) when there is one, else the RSA key from SubjectPublicKeyInfo: the
// rsaEncryption OID, then BIT STRING { SEQUENCE { INTEGER n, INTEGER e } } (smaller than the decoder)
bool tag(const uint8_t* der, size_t len, size_t& pos, uint8_t type, size_t& out) {
    if ((pos + 2 > len) || (der[pos] != type)) {
        return false;
    }

    out = der[pos + 1];
    pos += 2;
    if (out & 0x80) {
        size_t bytes = out & 0x7f;
        if ((bytes > 2) || (pos + bytes > len)) {
            return false;
        }

        out = 0;
        while (bytes--) {
            out = (out << 8) | der[pos++];
        }
    }

    return pos + out <= len;
}

bool rsaKey(const uint8_t* der, size_t len, br_rsa_public_key& out) {
    static const uint8_t Oid[] { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
    for (size_t pos = 0; pos + sizeof(Oid) < len; ++pos) {
        if (memcmp(der + pos, Oid, sizeof(Oid)) != 0) {
            continue;
        }

        pos += sizeof(Oid);
        if ((pos + 2 <= len) && (der[pos] == 0x05) && !der[pos + 1]) {
            pos += 2;
        }

        size_t size;
        if (!tag(der, len, pos, 0x03, size) || der[pos++] || !tag(der, len, pos, 0x30, size)
            || !tag(der, len, pos, 0x02, size))
        {
            return false;
        }

        out.n = const_cast<uint8_t*>(der + pos);
        out.nlen = size;
        pos += size;
        while (out.nlen && !*out.n) {
            ++out.n;
            --out.nlen;
        }

        if (!tag(der, len, pos, 0x02, size)) {
            return false;
        }

        out.e = const_cast<uint8_t*>(der + pos);
        out.elen = size;
        return out.nlen && size;
    }

    return false;
}

void insecureStartChain(const br_x509_class** ctx, const char*) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    self->size = 0;
    self->len = 0;
    self->first = true;
    self->mismatch = false;
}

void insecureStartCert(const br_x509_class** ctx, uint32_t length) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (!self->first) {
        return;
    }

    br_sha1_init(&self->sha);
    if (!self->cert && (length <= CertMax)) {
        self->cert = static_cast<uint8_t*>(malloc(length));
        self->size = self->cert ? length : 0;
    }
}

void insecureAppend(const br_x509_class** ctx, const unsigned char* buf, size_t len) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (!self->first) {
        return;
    }

    br_sha1_update(&self->sha, buf, len);
    if (self->cert && (self->len + len <= self->size)) {
        memcpy(self->cert + self->len, buf, len);
        self->len += len;
    }
}

void insecureEndCert(const br_x509_class** ctx) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (self->first && self->check) {
        uint8_t digest[br_sha1_SIZE];
        br_sha1_out(&self->sha, digest);
        self->mismatch = memcmp(digest, self->fingerprint, sizeof(digest)) != 0;
    }

    self->first = false;
}

unsigned insecureEndChain(const br_x509_class** ctx) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (self->mismatch) {
        return BR_ERR_X509_NOT_TRUSTED;
    }

    if (!self->cert || (self->len != self->size)) {
        return BR_ERR_X509_UNSUPPORTED;
    }

    free(self->key);
    self->key = tls::key(self->cert, self->len, &self->pkey);
    if (self->key) {
        return 0;
    }

    self->pkey.key_type = BR_KEYTYPE_RSA;
    return rsaKey(self->cert, self->len, self->pkey.key.rsa) ? 0 : BR_ERR_X509_UNSUPPORTED;
}

const br_x509_pkey* insecureGetPkey(const br_x509_class* const* ctx, unsigned* usages) {
    if (usages) {
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    }

    return &reinterpret_cast<const Insecure*>(ctx)->pkey;
}

const br_x509_class InsecureClass PROGMEM {
    sizeof(Insecure),
    insecureStartChain,
    insecureStartCert,
    insecureAppend,
    insecureEndCert,
    insecureEndChain,
    insecureGetPkey,
};

// the base ones; extend() replaces the list with its own (these included)
const uint16_t Suites[] PROGMEM {
    BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
};

} // namespace

// libs/tls_engine.h start(), of the image that includes this file
inline Failure start(Engine& engine, const char* host, const Options& options, const uint8_t* seed, size_t seed_len,
    unsigned char* input, size_t input_len, unsigned char* output, size_t output_len)
{
    auto* eng = &engine.sc.eng;
    br_ssl_client_zero(&engine.sc);
    br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);

    uint16_t suites[sizeof(Suites) / sizeof(Suites[0])];
    memcpy_P(suites, Suites, sizeof(Suites));
    br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));

    br_ssl_client_set_default_rsapub(&engine.sc);
    br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);

    // the smallest AES and GHASH of BearSSL (not constant-time, no secret worth a timing attack here)
    br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(eng, &br_aes_small_ctr_vtable);
    br_ssl_engine_set_ghash(eng, &br_ghash_ctmul);

    // the rest of TLS 1.2 where the image has it (ESPurna's tls module: ECDHE, ECDSA, ChaCha20)
    tls::extend(&engine.sc);

    if (options.ca) {
        // the chain against trust anchors
        auto* check = tls::caCheck(options.ca_pem, &engine.ca);
        if (!check) {
            return Failure::NoCa;
        }
        br_ssl_engine_set_x509(eng, check);
    } else {
        auto& x509 = engine.x509;
        x509.vtable = &InsecureClass;
        x509.check = options.check;
        memcpy(x509.fingerprint, options.fingerprint, sizeof(x509.fingerprint));
        br_ssl_engine_set_x509(eng, &x509.vtable);
    }

    // an input buffer below the full record size makes the client ask for max_fragment_length
    br_ssl_engine_set_buffers_bidi(eng, input, input_len, output, output_len);
    br_ssl_engine_inject_entropy(eng, seed, seed_len);

    if (!br_ssl_client_reset(&engine.sc, host, 0)) {
        return Failure::Tls;
    }

    return Failure::None;
}

} // namespace setup
} // namespace tls
} // namespace espurna
