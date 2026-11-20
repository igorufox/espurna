/*

Part of WEBSERVER MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

An output into the response of a request: what the callback prints is kept (up to a limit) and sent once it returns.
The handlers run in the loop (libs/http_server.h), the callback runs right away.

*/

#pragma once

#include "espurna.h"
#include "libs/http_server.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace espurna {
namespace web {
namespace print {

struct Config {
    const char* mimeType;
    size_t limit;           // bytes kept, the rest is dropped (a terminal command that never ends its output)
};

static constexpr Config DefaultConfig {
    "text/plain",
    16 * 1024,
};

class RequestPrint : public ::espurna::Output {
public:
    explicit RequestPrint(Config config) :
        _config(config)
    {}

    using ::espurna::Output::write;

    void write(const char* data, size_t size) override {
        const size_t room = (_out.size() < _config.limit) ? (_config.limit - _out.size()) : 0;
        const size_t take = std::min(room, size);
        _out.insert(_out.end(), data, data + take);
    }

    // 200 with everything printed
    void send(espurna::http::server::Request& request) {
        auto out = std::make_shared<std::vector<uint8_t>>(std::move(_out));
        request.send(200, _config.mimeType, static_cast<long>(out->size()),
            [out](uint8_t* buffer, size_t max, size_t index) -> size_t {
                const size_t take = (index < out->size()) ? std::min(max, out->size() - index) : 0;
                if (take) {
                    std::copy(out->data() + index, out->data() + index + take, buffer);
                }
                return take;
            });
    }

private:
    Config _config;
    std::vector<uint8_t> _out;
};

// runs `callback(::espurna::Output&)` and answers the request with what it printed
template <typename T>
void scheduleFromRequest(Config config, espurna::http::server::Request& request, T&& callback) {
    RequestPrint print(config);
    callback(static_cast<::espurna::Output&>(print));
    print.send(request);
}

template <typename T>
void scheduleFromRequest(espurna::http::server::Request& request, T&& callback) {
    scheduleFromRequest(DefaultConfig, request, std::forward<T>(callback));
}

} // namespace print
} // namespace web
} // namespace espurna
