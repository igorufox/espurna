/*

Part of the API MODULE

Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once

#include <Arduino.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "api_async_server.h"
#include "api_path.h"
#include "web.h"

namespace espurna {
namespace api {

// temporary object, which we can only create while doing the API dispatch

struct Request {
    Request() = delete;

    Request(const Request&) = default;
    Request& operator=(const Request&) = delete;

    Request(Request&&) noexcept = default;
    Request& operator=(Request&&) = delete;

    Request(WebRequest& request, const PathParts& pattern, const PathParts& parts) :
        _request(request),
        _pattern(pattern),
        _parts(parts)
    {}

    template <typename T>
    void handle(T&& handler) {
        if (_done) return;
        _done = true;
        handler(_request);
    }

    template <typename T>
    void param_foreach(T&& handler) {
        const size_t params { _request.params() };
        for (size_t current = 0; current < params; ++current) {
            handler(String(_request.paramName(current)), String(_request.paramValue(current)));
        }
    }

    template <typename T>
    void param_foreach(const String& name, T&& handler) {
        param_foreach([&](const String& param_name, const String& param_value) {
            if (param_name == name) {
                handler(param_value);
            }
        });
    }

    bool done() const {
        return _done;
    }

    const PathParts& parts() const {
        return _parts;
    }

    String part(size_t index) const {
        return _parts[index].toString();
    }

    // Only works when pattern cointains '+', retrieving the part at the same index from the real path
    // e.g. for the pair of `some/+/path` and `some/data/path`, calling `wildcard(0)` will return `data`
    // inline: the modules that register API paths (relay, sensors, light, rf, scheduler) work with it without being
    // linked against the api module (they call through the head, apiRegister())
    String wildcard(int index) const {
        return PathParts::wildcard(_pattern, _parts, index).toString();
    }

    size_t wildcards() const {
        return PathParts::wildcards(_pattern);
    }

    // Extract form data parameter value from request by name
    StringView param(const String& name) {
        const char* value = _request.param(name.c_str());
        return value ? StringView(value) : StringView();
    }

    // Send out the payload and finish the request
    // By default, payload is sent with status 200
    // For zero-length payloads, status is set to 204
    void send(const String& payload) {
        if (_done) {
            return;
        }

        _done = true;
        if (payload.length()) {
            webSend(_request, 200, "text/plain", payload);
        } else {
            _request.send(204, "", "");
        }
    }

private:
    bool _done { false };

    WebRequest& _request;
    const PathParts& _pattern;
    const PathParts& _parts;
};

struct RequestBuffer {
    RequestBuffer() = default;

    size_t size() const noexcept {
        return _data.size();
    }

    void append(uint8_t value) {
        _data.push_back(value);
    }

    void append(const uint8_t* data, size_t size) {
        _data.insert(_data.end(), data, data + size);
    }

    const uint8_t* data() const noexcept {
        return _data.data();
    }

    uint8_t* data() noexcept {
        return _data.data();
    }

    void reserve(size_t capacity) {
        _data.reserve(capacity);
    }

private:
    std::vector<uint8_t> _data;
};

using RequestBufferPtr = std::unique_ptr<RequestBuffer>;

struct RequestHelper {
    RequestHelper() = delete;

    RequestHelper(const RequestHelper&) = delete;
    RequestHelper& operator=(const RequestHelper&) = delete;

    RequestHelper(RequestHelper&&) noexcept = default;
    RequestHelper& operator=(RequestHelper&&) = delete;

    // &path is request.url(), valid throughout the request's lifetime
    RequestHelper(WebRequest& request, const PathParts& pattern) :
        _request(request),
        _pattern(pattern),
        _path(espurna::StringView(request.url())),
        _match(_pattern.match(_path))
    {}

    RequestBuffer& reserved_buffer(size_t capacity) {
        if (!_buffer) {
            _buffer = std::make_unique<RequestBuffer>();
            _buffer->reserve(capacity);
        }

        return *_buffer;
    }

    RequestBuffer& buffer() const {
        return *_buffer;
    }

    bool buffering() const {
        return static_cast<bool>(_buffer);
    }

    Request request() const {
        return Request(_request, _pattern, _path);
    }

    const PathParts& parts() const {
        return _path;
    }

    bool match() const {
        return _match;
    }

private:
    WebRequest& _request;

    RequestBufferPtr _buffer;

    const PathParts& _pattern;
    PathParts _path;
    bool _match;
};

} // namespace api
} // namespace espurna
