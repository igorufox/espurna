/*

The telnet_reverse module (docs/modules.md), every chip alike: the board opens a telnet session OUT to a listener, for a
board behind NAT or a firewall (start `ncat -vlp <port>` on a host the board reaches, then the command or the MQTT
topic below). Owner 2026-11-14: «делай как модуль - дико странная функция» - upstream had it off, it was never in our
builds, the head has none of it but the session itself (telnetReverse(), telnet.cpp: the same client as an accepted
one, the same limit).

    TELNET.REVERSE <HOST> <PORT>            terminal
    <root>/telnet_reverse/set  host:port    MQTT (an URL as well: telnet://host:port)

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "mqtt.h"
#include "telnet.h"
#include "terminal.h"
#include "libs/URL.h"

namespace {

#if TERMINAL_SUPPORT
PROGMEM_STRING(Reverse, "TELNET.REVERSE");

void reverse(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, "TELNET.REVERSE <HOST> <PORT>");
        return;
    }

    const auto port = espurna::settings::internal::convert<uint16_t>(ctx.argv[2]);
    if (!port) {
        terminalError(ctx, "Invalid port");
        return;
    }

    if (telnetReverse(ctx.argv[1].c_str(), port)) {
        terminalOK(ctx);
        return;
    }

    terminalError(ctx, "Unable to connect");
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Reverse, reverse},
};
#endif

#if MQTT_SUPPORT
// host:port, or an URL with them
void connectTo(espurna::string target) {
    if (target.indexOf("://") < 0) {
        target = espurna::string("telnet://") + target;
    }

    URL parsed(std::move(target));
    if (!parsed.host.length() || !parsed.port) {
        DEBUG_MSG_P(PSTR("[TELNET] Cannot parse the target\n"));
        return;
    }

    telnetReverse(parsed.host.c_str(), parsed.port);
}

void mqttCallback(unsigned int type, espurna::StringView topic, espurna::StringView payload) {
    switch (type) {
    case MQTT_CONNECT_EVENT:
        mqttSubscribe(MQTT_TOPIC_TELNET_REVERSE);
        break;

    case MQTT_MESSAGE_EVENT:
        if (mqttMagnitude(topic).equals(MQTT_TOPIC_TELNET_REVERSE)) {
            connectTo(payload.toString());
        }
        break;
    }
}
#endif

} // namespace

ESPURNA_MODULE(telnet_reverse, "telnet_reverse", 6) {
#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
#if MQTT_SUPPORT
    mqttRegister(mqttCallback);
#endif
}

#endif // MODULE_BUILD
