/*

MDNS MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

One responder on every chip (libs/mdns_responder.h): the host, ESPurna's services, the broker query of MQTT, and
Matter's services (the Matter library publishes through espurna_mdns_*). It replaced the three libraries of the
chips (LEAmDNS of the ESP8266 core, ESPmDNS on the IDF mdns component, the lwIP responder of LibreTiny; U28, W38)

*/

#include "espurna.h"

#if MDNS_SERVER_SUPPORT

#include "espurna_mdns.h"
#include "espurna_tasks.h"
#include "telnet.h"
#include "utils.h"
#include "web.h"

#define MDNS_LOCK ::espurna::system::Mutex
#define MDNS_LOG(fmt, ...) DEBUG_MSG_P(PSTR(fmt), ##__VA_ARGS__)
#include "libs/mdns_responder_impl.h"

namespace espurna {
namespace mdns {
namespace {

constexpr char Owner[] = "espurna";

void addServices() {
    namespace responder = ::espurna::mdns::responder;
    const auto hostname = systemHostname();

    responder::update(Owner);

#if WEB_SUPPORT
    responder::add(Owner, hostname.c_str(), "_http", true, webPort(), 0, nullptr, 0, nullptr, 0);
#endif

#if TELNET_SUPPORT
    responder::add(Owner, hostname.c_str(), "_telnet", true, telnetPort(), 0, nullptr, 0, nullptr, 0);
#endif

#if OTA_ARDUINOOTA_SUPPORT
    {
        const auto app = buildApp();
        const String entries[] {
            String(F("app_name=")) + String(app.name),
            String(F("app_version=")) + String(app.version),
            String(F("build_date=")) + String(app.build_time),
            String(F("mac=")) + String(systemChipId()),
            String(F("target_board=")) + String(systemDevice()),
            String(F("auth_upload=")) + ((systemPassword().length() > 0) ? F("yes") : F("no")),
        };
        const char* txt[sizeof(entries) / sizeof(entries[0])];
        for (size_t index = 0; index < (sizeof(entries) / sizeof(entries[0])); ++index) {
            txt[index] = entries[index].c_str();
        }
        responder::add(Owner, hostname.c_str(), "_arduino", true, OTA_PORT, 0,
            txt, sizeof(txt) / sizeof(txt[0]), nullptr, 0);
    }
#endif

    responder::commit(Owner);
}

void start() {
    const auto hostname = systemHostname();
    if (::espurna::mdns::responder::begin(hostname)) {
        DEBUG_MSG_P(PSTR("[MDNS] Started with hostname %s\n"), hostname.c_str());
        addServices();
        espurnaRegisterLoop(::espurna::mdns::responder::loop);
        return;
    }

    DEBUG_MSG_P(PSTR("[MDNS] ERROR\n"));
}

} // namespace
} // namespace mdns
} // namespace espurna

// a blocking query for the first broker (MQTT): the answers of one second
bool mdnsServiceQuery(const String& service, const String& protocol, MdnsServerQueryCallback callback) {
    return ::espurna::mdns::responder::query(service.c_str(), protocol.c_str(), 1000, callback);
}

bool mdnsRunning() {
    return ::espurna::mdns::responder::running();
}

void mdnsServerSetup() {
    ::espurna::mdns::start();
}

#endif // MDNS_SERVER_SUPPORT
