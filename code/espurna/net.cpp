/*

The network stack of the head (libs/net_impl.h: TCP, the stream with the records of TLS, the HTTP server): the one
implementation, every chip. The modules call the head's; the HTTP client is the http module (http.cpp). Making a TLS
connection (HTTP and MQTT) is the tls module's (module_tls.cpp), through the hook below

*/

#include "espurna.h"

#include "espurna_tasks.h"
#include "modules.h"

#define HTTP_CLIENT_AGENT "ESPurna"
#define NET_LOG(fmt, ...) DEBUG_MSG_P(PSTR("[NET] " fmt "\n"), ##__VA_ARGS__)
#define NET_LOCK ::espurna::system::Mutex
#include "libs/net_impl.h"

namespace espurna {
namespace tls {

// a TLS connection is the tls module's (libs/tls_setup.h): the head has the records only. Without the module a secure
// connection fails, plain ones work (Apply + reboot installs it)
Failure start(Engine& engine, const char* host, const Options& options, const uint8_t* seed, size_t seed_len,
    unsigned char* input, size_t input_len, unsigned char* output, size_t output_len)
{
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(start)>(modules::Api::TlsStart);
    return func ? func(engine, host, options, seed, seed_len, input, input_len, output, output_len) : Failure::NoTls;
#else
    return Failure::NoTls;
#endif
}

} // namespace tls
} // namespace espurna
