/*

TLS - the engine of one connection (libs/tls.h): the structures the stream (libs/tls_impl.h, in the head of every
image) and the setup of TLS (libs/tls_setup.h: ESPurna's tls module, code/migration-app) both see.

The head has the TCP connection, the plain stream and the records of an established TLS connection (BearSSL's
ssl_engine.c: buffers in and out). Everything that makes a TLS connection - the client, the handshake, RSA, AES-GCM,
the server's certificate - is the setup's: start() below. ESPurna's head asks the tls module for it through the module
API; without the module a secure connection fails with Failure::NoTls, plain ones work as before.

*/

#pragma once

#include "tls.h"

#include <cstdlib>

namespace espurna {
namespace tls {

// the server certificate (the first one) is kept for its key, the SHA-1 of it when the fingerprint is checked
constexpr size_t CertMax { 4096 };

struct Insecure {
    const br_x509_class* vtable;
    uint8_t* cert;
    size_t size;
    size_t len;
    br_x509_pkey pkey;
    void* key;          // the block of the X.509 decoder the key points into

    bool first;         // the server certificate is being received
    bool check;
    uint8_t fingerprint[FingerprintSize];
    br_sha1_context sha;
    bool mismatch;
};

// the engine (~4 KB) and its record buffers are two blocks, a fragmented heap has a better chance. The blocks the
// setup made (the certificate, the key, the chain check) go with it
struct Engine {
    ~Engine() {
        free(x509.cert);
        free(x509.key);
        free(ca);
    }

    br_ssl_client_context sc;
    Insecure x509;
    void* ca { nullptr };   // the block of the chain check
};

// The TLS of one connection: the client set up on `engine` (zeroed by the caller) with the buffers, the seed injected,
// reset for `host`. Failure::None - the handshake can run (the stream pumps the records). Defined once per image:
// ESPurna's head forwards it to the tls module (net.cpp), code/migration-app calls the setup itself
Failure start(Engine& engine, const char* host, const Options& options, const uint8_t* seed, size_t seed_len,
    unsigned char* input, size_t input_len, unsigned char* output, size_t output_len);

} // namespace tls
} // namespace espurna
