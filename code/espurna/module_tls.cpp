/*

The TLS module (docs/modules.md), every chip alike: the TLS session of the head's one stream (libs/tls.h: the HTTP
client and MQTT) for any server (the NSPanel's weather, URL tiles, ThingSpeak, InfluxDB): mbedtls of the chip's SDK
(libs/tls_mbedtls.h) - the handshake, the records, the server's certificate (its SHA-1 fingerprint, or its chain:
MQTT mqttScCheck 2). The head asks for a session through one hook, Api::TlsStart; without the module a secure
connection fails (owner 2026-11-14: all of TLS out of the head; 2026-11-18: one crypto, mbedtls).

Installed (Apply + reboot), every https connection of the head's client gets its session here. The module also has
the MFLN.PROBE terminal command (does a server take smaller TLS records, mqttScMFLN). Starts before the modules
that use it.

*/

#include "espurna.h"

#if MODULE_BUILD

#include "modules.h"
#include "libs/tls_mbedtls.h"
#include "libs/URL.h"

#include <memory>

#if TERMINAL_SUPPORT
namespace {

// MFLN.PROBE <URL> <SIZE>: does the server take TLS records this small (max_fragment_length, mqttScMFLN)? The one TLS
// (libs/tls.h) connects with an input buffer of that size; the answer comes in the log when the handshake ended
PROGMEM_STRING(MflnProbe, "MFLN.PROBE");

struct Probe {
    espurna::tls::Stream stream;
    espurna::string host;
    size_t size;
};

std::unique_ptr<Probe> probe;

void probe_loop() {
    if (!probe) {
        return;
    }

    if (probe->stream.ready()) {
        DEBUG_MSG_P(PSTR("[NET] %s takes TLS records of %u bytes\n"),
            probe->host.c_str(), static_cast<unsigned>(probe->size));
        probe.reset();
    } else if (!probe->stream.pending()) {
        DEBUG_MSG_P(PSTR("[NET] %s: no TLS with records of %u bytes (error %d)\n"),
            probe->host.c_str(), static_cast<unsigned>(probe->size), probe->stream.tlsError());
        probe.reset();
    }
}

void mfln_probe(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, "MFLN.PROBE <URL> <SIZE>");
        return;
    }

    if (probe) {
        terminalError(ctx, "A probe is running");
        return;
    }

    URL parsed(std::move(ctx.argv[1]));
    const auto size = espurna::settings::internal::convert<uint16_t>(ctx.argv[2]);
    if (!parsed.host.length() || ((size != 512) && (size != 1024) && (size != 2048) && (size != 4096))) {
        terminalError(ctx, "MFLN.PROBE <URL> <512|1024|2048|4096>");
        return;
    }

    probe.reset(new (std::nothrow) Probe());
    if (!probe) {
        terminalError(ctx, "Out of memory");
        return;
    }

    probe->host = parsed.host;
    probe->size = size;

    espurna::tls::Options options;
    options.secure = true;
    options.fragment = size;
    if (!probe->stream.connect(parsed.host.c_str(), parsed.port ? parsed.port : 443, options)) {
        probe.reset();
        terminalError(ctx, "Cannot connect");
        return;
    }

    ctx.output.printf_P(PSTR("Probing %s, the result goes to the log\n"), parsed.host.c_str());
    terminalOK(ctx);
}


static constexpr ::terminal::Command Commands[] PROGMEM {
    {MflnProbe, mfln_probe},
};

} // namespace
#endif

ESPURNA_MODULE(tls, "tls", 4) {
    using espurna::modules::Api;

    modulesProvide(Api::TlsStart, &espurna::tls::mbedtls::open);

#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
    espurnaRegisterLoop(probe_loop);
#endif
}

#endif
