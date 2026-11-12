/*

The TLS module (docs/modules.md), every chip alike: the rest of TLS 1.2 for the head's one TLS (libs/tls.h: the
HTTP client and MQTT), for any server (the NSPanel's weather, URL tiles, ThingSpeak, InfluxDB): ECDHE with RSA and
ECDSA certificates, ChaCha20-Poly1305, the X.509 decoder. The code is libs/tls_full.h (the ESP32 safeboot has
the same); this module hands it to the head through three hooks: Api::TlsExtend, Api::TlsKey and Api::TlsCaCheck
(the chain check of MQTT mqttScCheck 2).
Installed (Apply + reboot), every https connection of the head's client offers its suites. The module also has
the MFLN.PROBE terminal command (does a server take smaller TLS records, mqttScMFLN). Starts before the modules
that use it.

*/

#include "espurna.h"

#if MODULE_BUILD

#include "modules.h"
#include "libs/tls_full.h"
#include "libs/URL.h"

#include <memory>

#if TERMINAL_SUPPORT
namespace {

// MFLN.PROBE <URL> <SIZE>: does the server take TLS records this small (max_fragment_length, mqttScMFLN)? The one TLS
// (libs/tls.h) connects with an input buffer of that size; the answer comes in the log when the handshake ended
PROGMEM_STRING(MflnProbe, "MFLN.PROBE");

struct Probe {
    espurna::tls::Stream stream;
    String host;
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
        DEBUG_MSG_P(PSTR("[NET] %s: no TLS with records of %u bytes (BearSSL error %d)\n"),
            probe->host.c_str(), static_cast<unsigned>(probe->size), probe->stream.tlsError());
        probe.reset();
    }
}

void mfln_probe(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, F("MFLN.PROBE <URL> <SIZE>"));
        return;
    }

    if (probe) {
        terminalError(ctx, F("A probe is running"));
        return;
    }

    URL parsed(std::move(ctx.argv[1]));
    const auto size = espurna::settings::internal::convert<uint16_t>(ctx.argv[2]);
    if (!parsed.host.length() || ((size != 512) && (size != 1024) && (size != 2048) && (size != 4096))) {
        terminalError(ctx, F("MFLN.PROBE <URL> <512|1024|2048|4096>"));
        return;
    }

    probe.reset(new (std::nothrow) Probe());
    if (!probe) {
        terminalError(ctx, F("Out of memory"));
        return;
    }

    probe->host = parsed.host;
    probe->size = size;

    espurna::tls::Options options;
    options.secure = true;
    options.fragment = size;
    if (!probe->stream.connect(parsed.host.c_str(), parsed.port ? parsed.port : 443, options)) {
        probe.reset();
        terminalError(ctx, F("Cannot connect"));
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

    modulesProvide(Api::TlsExtend, &espurna::tls::full::extend);
    modulesProvide(Api::TlsKey, &espurna::tls::full::key);
    modulesProvide(Api::TlsCaCheck, &espurna::tls::full::caCheck);

#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
    espurnaRegisterLoop(probe_loop);
#endif
}

#endif
