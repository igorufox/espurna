/*

The TLS module (docs/modules.md), every chip alike: the rest of TLS 1.2 for the head's one HTTP client
(libs/http_client.h), for any server (the NSPanel's weather, URL tiles, ThingSpeak, InfluxDB): ECDHE with RSA and
ECDSA certificates, ChaCha20-Poly1305, the X.509 decoder. The code is libs/http_client_tls.h (the ESP32 safeboot has
the same); this module only hands it to the head through the two hooks (Api::HttpTlsExtend, Api::HttpTlsKey).
Installed (Apply + reboot), every https connection of the head's client offers its suites. Starts before the modules
that use it.

*/

#include "espurna.h"

#if MODULE_BUILD

#include "modules.h"
#include "libs/http_client_tls.h"

ESPURNA_MODULE(tls, "tls", 4) {
    using espurna::modules::Api;

    modulesProvide(Api::HttpTlsExtend, &espurna::http::tls::full::extend);
    modulesProvide(Api::HttpTlsKey, &espurna::http::tls::full::key);
}

#endif
