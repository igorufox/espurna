/*

The network stack of the head (libs/net_impl.h: TCP, TLS, the HTTP client, the HTTP server): the one implementation,
every chip. The modules call the head's; the tls module (module_tls.cpp) extends the TLS (HTTP and MQTT) through the two
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
namespace tls {

// the tls module's, when installed (Apply + reboot)
bool extend(br_ssl_client_context* sc) {
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(extend)>(modules::Api::TlsExtend);
    return func ? func(sc) : false;
#else
    return false;
#endif
}

void* key(const uint8_t* der, size_t len, br_x509_pkey* out) {
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(key)>(modules::Api::TlsKey);
    return func ? func(der, len, out) : nullptr;
#else
    return nullptr;
#endif
}

const br_x509_class** caCheck(const char* pem, void** block) {
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(caCheck)>(modules::Api::TlsCaCheck);
    return func ? func(pem, block) : nullptr;
#else
    return nullptr;
#endif
}

} // namespace tls
} // namespace espurna
