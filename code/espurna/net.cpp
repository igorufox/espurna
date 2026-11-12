/*

The network stack of the head (libs/net_impl.h: TCP, the HTTP client, the HTTP server): the one implementation,
every chip. The modules call the head's; the tls module (module_tls.cpp) extends the client's TLS through the two
hooks below

*/

#include "espurna.h"

#include "espurna_tasks.h"
#include "modules.h"

#define HTTP_CLIENT_AGENT "ESPurna"
#define NET_LOG(fmt, ...) DEBUG_MSG_P(PSTR("[NET] " fmt "\n"), ##__VA_ARGS__)
#define NET_LOCK ::espurna::system::Mutex
#include "libs/net_impl.h"

namespace espurna {
namespace http {
namespace tls {

// the tls module's, when installed (Apply + reboot)
bool extend(br_ssl_client_context* sc) {
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(extend)>(modules::Api::HttpTlsExtend);
    return func ? func(sc) : false;
#else
    return false;
#endif
}

void* key(const uint8_t* der, size_t len, br_x509_pkey* out) {
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(key)>(modules::Api::HttpTlsKey);
    return func ? func(der, len, out) : nullptr;
#else
    return nullptr;
#endif
}

} // namespace tls
} // namespace http
} // namespace espurna
