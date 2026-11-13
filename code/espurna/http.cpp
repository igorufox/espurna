/*

The http module (docs/modules.md): the one http:// and https:// client (libs/http_client.h), every chip alike. Its code
is libs/http_client_impl.h, the same that code/migration-app and code/safeboot-app include; the connection and TLS under
it are the head's (libs/tcp.h, libs/tls.h through net.cpp). The modules that make requests need it (head.h): nspanel,
thingspeak, influxdb, and link against it.

*/

#if MODULE_BUILD

#include "espurna.h"

#include "espurna_tasks.h"

#define HTTP_CLIENT_AGENT "ESPurna"
#define NET_LOG(fmt, ...) DEBUG_MSG_P(PSTR("[NET] " fmt "\n"), ##__VA_ARGS__)
#define NET_LOCK ::espurna::system::Mutex
#include "libs/http_client_impl.h"

#endif // MODULE_BUILD
