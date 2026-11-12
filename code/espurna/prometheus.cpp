/*

PROMETHEUS METRICS MODULE

Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if WEB_SUPPORT && PROMETHEUS_SUPPORT

#include "prometheus.h"

#include "api.h"
#include "relay.h"
#include "sensor.h"
#include "web.h"

#include <cmath>

namespace espurna {
namespace prometheus {
namespace build {
namespace {

constexpr bool relaySupport() {
    return 1 == RELAY_SUPPORT;
}

constexpr bool sensorSupport() {
    return 1 == SENSOR_SUPPORT;
}

static_assert(relaySupport() || sensorSupport(), "");

} // namespace
} // namespace build

namespace {

void handler(WebRequest& request) {
    String out;
    char line[64];

    if (build::relaySupport()) {
        for (size_t index = 0; index < relayCount(); ++index) {
            const auto status = relayStatus(index);
            snprintf_P(line, sizeof(line), PSTR("relay%u %d\n"), index, static_cast<int>(status));
            out += line;
        }
    }

    if (build::sensorSupport()) {
        for (size_t index = 0; index < magnitudeCount(); ++index) {
            auto value = magnitudeValue(index);
            if (value) {
                value.topic.replace("/", "");
                out += value.topic;
                out += ' ';
                out += value.repr;
                out += '\n';
            }
        }
    }

    out += '\n';
    webSend(request, 200, "text/plain", out);
}

void setup() {
#if API_SUPPORT
    apiRegister(F("metrics"),
        [](ApiRequest& request) {
            request.handle(handler);
            return true;
        },
        nullptr
    );
#else
    webRequestRegister([](WebRequest& request) {
        if (!strcmp(request.url(), API_BASE_PATH "metrics")) {
            if (apiAuthenticate(request)) {
                handler(request);
                return true;
            }
            request.send(403, "text/plain", "");
            return true;
        }

        return false;
    });
#endif
}

} // namespace
} // namespace prometheus
} // namespace espurna

void prometheusSetup() {
    espurna::prometheus::setup();
}

#endif // PROMETHEUS_SUPPORT
