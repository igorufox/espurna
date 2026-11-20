/*

TLS - the session of one connection on mbedtls (libs/tls.h Session), the tls module's (module_tls.cpp): the handshake,
the records, the server's certificate. mbedTLS 3.6.6 on every chip (code/libraries.ini; ESP-IDF, the ESP8266 SDK and the Beken BDK build it):
the 3.x calls. The I/O is the TCP connection of the head's stream (libs/tcp.h), from the loop, never
waiting: the session answers "not yet" until the bytes are there.

- the server: not checked (the caller checks a download by its SHA-256), the SHA-1 of its certificate (Options::check),
  or its chain against the CA certificates of Options::ca_pem, else the built-in root (ISRG Root X1 of Let's Encrypt)
- the records: Options::fragment asks the server for smaller ones (max_fragment_length)

*/

#pragma once

#include "tls.h"
#include "tcp.h"
#include "hash.h"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/version.h>

#include <cstring>
#include <new>

#if defined(ESP_PLATFORM) || defined(ESP8266)
// esp_fill_random(): ESP-IDF esp_random.h, ESP8266_RTOS_SDK esp_system.h
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif
#elif defined(BK72XX)
#include "../arch/beken/beken_api.h"
#endif

namespace espurna {
namespace tls {
namespace mbedtls {

// ISRG Root X1 (Let's Encrypt): the chain check without Options::ca_pem
static constexpr char BuiltinRoot[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAwTzELMAkGA1UE\n"
"BhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2VhcmNoIEdyb3VwMRUwEwYDVQQD\n"
"EwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQG\n"
"EwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMT\n"
"DElTUkcgUm9vdCBYMTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54r\n"
"Vygch77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+0TM8ukj1\n"
"3Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6UA5/TR5d8mUgjU+g4rk8K\n"
"b4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sWT8KOEUt+zwvo/7V3LvSye0rgTBIlDHCN\n"
"Aymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyHB5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ\n"
"4Q7e2RCOFvu396j3x+UCB5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf\n"
"1b0SHzUvKBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWnOlFu\n"
"hjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTnjh8BCNAw1FtxNrQH\n"
"usEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbwqHyGO0aoSCqI3Haadr8faqU9GY/r\n"
"OPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CIrU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4G\n"
"A1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY\n"
"9umbbjANBgkqhkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ3BebYhtF8GaV\n"
"0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KKNFtY2PwByVS5uCbMiogziUwt\n"
"hDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJw\n"
"TdwJx4nLCgdNbOhdjsnvzqvHu7UrTkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nx\n"
"e5AW0wdeRlN8NwdCjNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZA\n"
"JzVcoyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq4RgqsahD\n"
"YVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPAmRGunUHBcnWEvgJBQl9n\n"
"JEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57demyPxgcYxn/eR44/KJ4EBs+lVDR3veyJ\n"
"m+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

// the hardware random of the chip
int random(void*, unsigned char* out, size_t len) {
#if defined(ESP_PLATFORM) || defined(ESP8266)
    esp_fill_random(out, len);
#elif defined(BK72XX)
    ::espurna::beken::rand_bytes(out, len);
#endif
    return 0;
}

int send(void* context, const unsigned char* data, size_t len) {
    auto& tcp = *static_cast<tcp::Connection*>(context);
    if (tcp.closed()) {
        return -0x004E;     // MBEDTLS_ERR_NET_SEND_FAILED
    }

    const auto sent = tcp.write(data, len);
    return sent ? static_cast<int>(sent) : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int receive(void* context, unsigned char* out, size_t len) {
    auto& tcp = *static_cast<tcp::Connection*>(context);
    const auto got = tcp.read(out, len);
    if (got) {
        return static_cast<int>(got);
    }

    // the peer closed and everything it sent was read: the end of the stream
    return tcp.closed() ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
}

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
unsigned char fragmentCode(size_t fragment) {
    switch (fragment) {
    case 512:
        return MBEDTLS_SSL_MAX_FRAG_LEN_512;
    case 1024:
        return MBEDTLS_SSL_MAX_FRAG_LEN_1024;
    case 2048:
        return MBEDTLS_SSL_MAX_FRAG_LEN_2048;
    case 4096:
        return MBEDTLS_SSL_MAX_FRAG_LEN_4096;
    }

    return MBEDTLS_SSL_MAX_FRAG_LEN_NONE;
}
#endif

class Connection : public Session {
public:
    Connection() {
        mbedtls_ssl_init(&_ssl);
        mbedtls_ssl_config_init(&_config);
        mbedtls_x509_crt_init(&_ca);
    }

    ~Connection() override {
        mbedtls_ssl_free(&_ssl);
        mbedtls_ssl_config_free(&_config);
        mbedtls_x509_crt_free(&_ca);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 0 - the session can run its handshake; else the mbedtls error, `failure` set
    int setup(tcp::Connection& tcp, const char* host, const Options& options, Failure& failure) {
        failure = Failure::Tls;

        int result = mbedtls_ssl_config_defaults(&_config, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (result) {
            return result;
        }

        mbedtls_ssl_conf_rng(&_config, random, nullptr);

        _check = options.check;
        if (_check) {
            std::memcpy(_fingerprint, options.fingerprint, sizeof(_fingerprint));
        }

        _ca_check = options.ca;
        if (_ca_check) {
            const char* pem = (options.ca_pem && *options.ca_pem) ? options.ca_pem : BuiltinRoot;
            // PEM: the length counts the terminating zero
            result = mbedtls_x509_crt_parse(&_ca,
                reinterpret_cast<const unsigned char*>(pem), std::strlen(pem) + 1);
            if (result < 0) {
                failure = Failure::NoCa;
                return result;
            }

            mbedtls_ssl_conf_ca_chain(&_config, &_ca, nullptr);
            mbedtls_ssl_conf_authmode(&_config, MBEDTLS_SSL_VERIFY_REQUIRED);
        } else {
            // the certificate is read (its fingerprint), its chain is not checked
            mbedtls_ssl_conf_authmode(&_config, MBEDTLS_SSL_VERIFY_OPTIONAL);
        }

        mbedtls_ssl_conf_verify(&_config, verifyCallback, this);

#if defined(MBEDTLS_SSL_MAX_FRAGMENT_LENGTH)
        if (options.fragment) {
            mbedtls_ssl_conf_max_frag_len(&_config, fragmentCode(options.fragment));
        }
#endif

        result = mbedtls_ssl_setup(&_ssl, &_config);
        if (result) {
            failure = (result == MBEDTLS_ERR_SSL_ALLOC_FAILED) ? Failure::Memory : Failure::Tls;
            return result;
        }

        result = mbedtls_ssl_set_hostname(&_ssl, host);
        if (result) {
            return result;
        }

        mbedtls_ssl_set_bio(&_ssl, &tcp, send, receive, nullptr);
        failure = Failure::None;
        return 0;
    }

    int handshake() override {
        const int result = mbedtls_ssl_handshake(&_ssl);
        if (!result) {
            return 1;
        }

        if ((result == MBEDTLS_ERR_SSL_WANT_READ) || (result == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            return 0;
        }

        return (result < 0) ? result : -1;
    }

    int read(uint8_t* out, size_t len) override {
        const int result = mbedtls_ssl_read(&_ssl, out, len);
        if (result > 0) {
            return result;
        }

        if ((result == MBEDTLS_ERR_SSL_WANT_READ) || (result == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            return 0;
        }

        // 0: the end of the stream without a close notify
        return result ? result : MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }

    int write(const uint8_t* data, size_t len) override {
        const int result = mbedtls_ssl_write(&_ssl, data, len);
        if (result >= 0) {
            return result;
        }

        if ((result == MBEDTLS_ERR_SSL_WANT_READ) || (result == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            return 0;
        }

        return result;
    }

    void close() override {
        mbedtls_ssl_close_notify(&_ssl);
    }

    Failure verify() const override {
        if (_mismatch) {
            return Failure::Fingerprint;
        }

        if (_ca_check && _untrusted) {
            return Failure::Untrusted;
        }

        return Failure::None;
    }

private:
    // the server certificate (depth 0): its SHA-1 against the fingerprint; with the chain check, what mbedtls found
    static int verifyCallback(void* context, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
        auto& self = *static_cast<Connection*>(context);

        if (!self._ca_check) {
            *flags = 0;
        } else if (*flags) {
            self._untrusted = true;
        }

        if (self._check && (depth == 0)) {
            hash::Sha1 sha;
            sha.update(crt->raw.p, crt->raw.len);
            uint8_t digest[hash::Sha1::Size];
            sha.finish(digest);
            if (std::memcmp(digest, self._fingerprint, sizeof(digest)) != 0) {
                self._mismatch = true;
                return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
            }
        }

        return 0;
    }

    mbedtls_ssl_context _ssl;
    mbedtls_ssl_config _config;
    mbedtls_x509_crt _ca;

    bool _check { false };
    uint8_t _fingerprint[FingerprintSize] {};
    bool _ca_check { false };

    bool _mismatch { false };
    bool _untrusted { false };
};

// tls::open() of the head (net.cpp), through the module API
Session* open(tcp::Connection& tcp, const char* host, const Options& options, Failure& failure, int& error) {
    auto* out = new (std::nothrow) Connection();
    if (!out) {
        failure = Failure::Memory;
        error = 0;
        return nullptr;
    }

    error = out->setup(tcp, host, options, failure);
    if (error) {
        delete out;
        return nullptr;
    }

    return out;
}

} // namespace mbedtls
} // namespace tls
} // namespace espurna
