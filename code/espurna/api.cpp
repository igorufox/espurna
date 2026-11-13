/*

API & WEB API MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020-2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

// -----------------------------------------------------------------------------

#include "espurna.h"

#if API_SUPPORT
#include "api.h"
#endif

#if WEB_SUPPORT
#include <ArduinoJson.h>

#include "web.h"
#endif

#include <algorithm>
#include <memory>
#include <cstring>
#include <forward_list>
#include <vector>

#include "system.h"
#include "rpc.h"

#include "api_path.h"

// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------

#if API_SUPPORT
namespace espurna {
namespace api {
namespace content_type {
namespace {

STRING_VIEW_INLINE(Anything, "*/*");
STRING_VIEW_INLINE(Text, "text/plain");
STRING_VIEW_INLINE(Json, "application/json");
STRING_VIEW_INLINE(Form, "application/x-www-form-urlencoded");

} // namespace
} // namespace content_type

namespace {

bool accepts(WebRequest& request, StringView pattern) {
    const char* header = request.header("Accept");
    if (!header) {
        return true;
    }

    const String value(header);
    return (value.indexOf(StringView(content_type::Anything).toString()) >= 0)
        || (value.indexOf(pattern.toString()) >= 0);
}

bool accepts_text(WebRequest& request) {
    return accepts(request, content_type::Text);
}

bool accepts_json(WebRequest& request) {
    return accepts(request, content_type::Json);
}

bool is_content_type(WebRequest& request, StringView value) {
    return String(request.contentType()).startsWith(value.toString());
}

bool is_form_data(WebRequest& request) {
    return is_content_type(request, content_type::Form);
}

bool is_json(WebRequest& request) {
    return is_content_type(request, content_type::Json);
}

class BaseWebHandler;

// The state of an API request between its body pieces and its handler (the request's context, freed with it): the
// handler that took it and the path parts of the request
struct Context {
    BaseWebHandler* handler { nullptr };
    bool resolved { false };
    std::unique_ptr<RequestHelper> helper;
};

Context* context(WebRequest& request) {
    auto* out = static_cast<Context*>(request.context());
    if (!out) {
        out = new (std::nothrow) Context();
        request.setContext(out, [](void* ptr) {
            delete static_cast<Context*>(ptr);
        });
    }
    return out;
}

void attach_helper(WebRequest& request, RequestHelper&& helper) {
    auto* ctx = context(request);
    if (ctx) {
        ctx->helper.reset(new (std::nothrow) RequestHelper(std::move(helper)));
    }
}

RequestHelper* helper_of(WebRequest& request) {
    auto* ctx = static_cast<Context*>(request.context());
    return (ctx && ctx->helper) ? ctx->helper.get() : nullptr;
}

class BaseWebHandler {
public:
    BaseWebHandler() = delete;
    virtual ~BaseWebHandler() = default;

    BaseWebHandler(const BaseWebHandler&) = delete;
    BaseWebHandler& operator=(const BaseWebHandler&) = delete;

    BaseWebHandler(BaseWebHandler&&) = delete;
    BaseWebHandler& operator=(BaseWebHandler&&) = delete;

    // In case this needs to be copied or moved, ensure PathParts copy references the new object's string
    template <typename T,
              typename = typename std::enable_if<
                  std::is_constructible<String, T>::value>::type>
    explicit BaseWebHandler(T&& pattern) :
        _pattern(std::forward<T>(pattern)),
        _parts(_pattern)
    {}

    const String& pattern() const {
        return _pattern;
    }

    const PathParts& parts() const {
        return _parts;
    }

    // takes the request (and attaches its helper)
    virtual bool canHandle(WebRequest& request) = 0;
    // a piece of a body that is not a small form (JSON)
    virtual void handleBody(WebRequest&, const uint8_t*, size_t, size_t, size_t) {
    }
    virtual void handleRequest(WebRequest& request) = 0;

private:
    String _pattern;
    PathParts _parts;
};

// 'Modernized' API configuration:
// - `Api-Key` header for both GET and PUT
// - the request body parsed as a JSON object, up to BodyMax bytes
// - Same as the text/plain, when ApiRequest::handle was not called it will then call GET

class JsonWebHandler final : public BaseWebHandler {
public:
    static constexpr size_t BufferSize { API_JSON_BUFFER_SIZE };
    static constexpr size_t BodyMax { 1024 };

    JsonWebHandler() = delete;

    JsonWebHandler(const JsonWebHandler&) = delete;
    JsonWebHandler& operator=(const JsonWebHandler&) = delete;

    JsonWebHandler(JsonWebHandler&&) = delete;
    JsonWebHandler& operator=(JsonWebHandler&&) = delete;

    template <typename Path, typename Get, typename Put>
    JsonWebHandler(Path&& path, Get&& get, Put&& put) :
        BaseWebHandler(std::forward<Path>(path)),
        _get(std::forward<Get>(get)),
        _put(std::forward<Put>(put))
    {}

    bool canHandle(WebRequest& request) override {
        if (!apiEnabled()) {
            return false;
        }

        auto helper = RequestHelper(request, parts());
        if (helper.match() && apiAuthenticate(request)) {
            switch (request.method()) {
            case WebMethod::Head:
                break;
            case WebMethod::Put:
                if (!is_json(request) || !_put || !_get) {
                    return false;
                }
                break;
            case WebMethod::Get:
                if (!_get) {
                    return false;
                }
                break;
            default:
                return false;
            }
            attach_helper(request, std::move(helper));
            return true;
        }

        return false;
    }

    void _handleGet(WebRequest& request, Request& apireq) {
        DynamicJsonBuffer jsonBuffer(BufferSize);
        JsonObject& root = jsonBuffer.createObject();
        if (!_get(apireq, root)) {
            request.send(500, "text/plain", "");
            return;
        }

        if (!apireq.done()) {
            String out;
            root.printTo(out);
            webSend(request, 200, "application/json", std::move(out));
            return;
        }

        if (!request.answered()) {
            request.send(500, "text/plain", "");
        }
    }

    void _handlePut(WebRequest& request, const uint8_t* data, size_t size) {
        auto* ptr = reinterpret_cast<const char*>(data);
        auto reader = StringView(ptr, ptr + size);

        DynamicJsonBuffer jsonBuffer(BufferSize);
        JsonObject& root = jsonBuffer.parseObject(reader);
        if (!root.success()) {
            request.send(500, "text/plain", "");
            return;
        }

        auto* helper = helper_of(request);
        if (!helper) {
            request.send(500, "text/plain", "");
            return;
        }

        auto apireq = helper->request();
        if (!_put(apireq, root)) {
            if (!request.answered()) {
                request.send(500, "text/plain", "");
            }
            return;
        }

        if (!apireq.done()) {
            _handleGet(request, apireq);
        }
    }

    void handleBody(WebRequest& request, const uint8_t* data, size_t len, size_t index, size_t total) override {
        auto* helper = helper_of(request);
        if (!helper || (total > BodyMax)) {
            return;
        }

        auto& buffer = helper->reserved_buffer(total);
        if (!index) {
            buffer = RequestBuffer();
            buffer.reserve(total);
        }
        buffer.append(data, len);
    }

    void handleRequest(WebRequest& request) override {
        if (!accepts_json(request)) {
            webSend(request, 406, "text/plain", content_type::Json.toString());
            return;
        }

        auto* helper = helper_of(request);
        if (!helper) {
            request.send(500, "text/plain", "");
            return;
        }

        switch (request.method()) {
        case WebMethod::Head:
            request.send(204, "", "");
            return;

        case WebMethod::Get: {
            auto apireq = helper->request();
            _handleGet(request, apireq);
            return;
        }

        case WebMethod::Put:
            if (!helper->buffering() || !helper->buffer().size()) {
                request.send(400, "text/plain", "");
                return;
            }
            _handlePut(request, helper->buffer().data(), helper->buffer().size());
            return;

        default:
            request.send(405, "text/plain", "");
            break;
        }
    }

private:
    JsonHandler _get;
    JsonHandler _put;
};

// ESPurna legacy API configuration
// - ?apikey=... to authorize in GET or PUT
// - ?anything=... for input data (common key is "value"), the query or a form body

class BasicWebHandler final : public BaseWebHandler {
public:
    template <typename Path, typename Get, typename Put>
    BasicWebHandler(Path&& path, Get&& get, Put&& put) :
        BaseWebHandler(std::forward<Path>(path)),
        _get(std::forward<Get>(get)),
        _put(std::forward<Put>(put))
    {}

    bool canHandle(WebRequest& request) override {
        if (!apiEnabled()) {
            return false;
        }

        switch (request.method()) {
        case WebMethod::Head:
        case WebMethod::Get:
            break;
        case WebMethod::Put:
            if (!is_form_data(request)) {
                return false;
            }
            break;
        default:
            return false;
        }

        auto helper = RequestHelper(request, parts());
        if (helper.match()) {
            attach_helper(request, std::move(helper));
            return true;
        }

        return false;
    }

    void handleRequest(WebRequest& request) override {
        if (!apiAuthenticate(request)) {
            request.send(403, "text/plain", "");
            return;
        }

        if (!accepts_text(request)) {
            webSend(request, 406, "text/plain", content_type::Text.toString());
            return;
        }

        const auto method = request.method();
        const auto is_put =
            (!apiRestFul() || (WebMethod::Put == method))
            && _check_unhandled_params(request);

        switch (method) {
        case WebMethod::Head:
            request.send(204, "", "");
            return;

        case WebMethod::Get:
        case WebMethod::Put: {
            auto* helper = helper_of(request);
            if (!helper) {
                request.send(500, "text/plain", "");
                return;
            }

            auto apireq = helper->request();
            if (is_put) {
                if (!_put(apireq)) {
                    request.send(500, "text/plain", "");
                    return;
                }

                if (apireq.done()) {
                    return;
                }
            }

            if (!_get(apireq)) {
                if (!request.answered()) {
                    request.send(500, "text/plain", "");
                }
                return;
            }

            if (!apireq.done()) {
                request.send(204, "", "");
                return;
            }

            break;
        }

        default:
            request.send(405, "text/plain", "");
            break;
        }
    }

    const BasicHandler& get() const {
        return _get;
    }

    const BasicHandler& put() const {
        return _put;
    }

private:
    bool _check_unhandled_params(WebRequest& request) {
        const auto params = request.params();
        for (size_t param = 0; param < params; ++param) {
            if (!apiReservedParam(StringView(request.paramName(param)))) {
                return true;
            }
        }

        return false;
    }

    BasicHandler _get;
    BasicHandler _put;
};

namespace internal {

std::forward_list<BaseWebHandler*> list;

// API off (the default): a web handler with its parsed path costs ~200 bytes of heap, ~20 of them are registered.
// Until the API is enabled only the path and the callbacks are kept, the handlers are made on the settings reload
// that enables it
template <typename Handler>
struct Pending {
    String path;
    Handler get;
    Handler put;
};

std::forward_list<Pending<BasicHandler>> pending_basic;
std::forward_list<Pending<JsonHandler>> pending_json;

bool routed { false };

} // namespace internal

namespace simple {

bool ok(Request& request) {
    STRING_VIEW_INLINE(Ok, "OK");
    request.send(Ok.toString());
    return true;
}

bool error(ApiRequest& request) {
    STRING_VIEW_INLINE(Error, "ERROR");
    request.send(Error.toString());
    return true;
}

} // namespace simple

STRING_VIEW_INLINE(BasePath, API_BASE_PATH);

// the handler that takes this request, found once (in registration order)
BaseWebHandler* resolve(WebRequest& request) {
    auto* ctx = context(request);
    if (!ctx) {
        return nullptr;
    }

    if (!ctx->resolved) {
        ctx->resolved = true;
        for (auto* handler : internal::list) {
            if (handler->canHandle(request)) {
                ctx->handler = handler;
                break;
            }
        }
    }

    return ctx->handler;
}

void onBody(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    auto* handler = resolve(request);
    if (handler) {
        handler->handleBody(request, data, len, index, request.contentLength());
    }
}

void onRequest(WebRequest& request) {
    auto* handler = resolve(request);
    if (!handler) {
        request.send(404, "text/plain", "Not Found");
        return;
    }

    handler->handleRequest(request);
}

void add(BaseWebHandler* ptr) {
    // one route of the web server for every API path
    if (!internal::routed) {
        internal::routed = true;
        const auto prefix = BasePath.toString() + '*';
        webServer().on(prefix.c_str(), WebMethod::Any, onRequest, onBody);
    }

    internal::list.emplace_front(ptr);
}

template <typename Handler, typename Get, typename Put>
void add(String path, Get&& get, Put&& put) {
    add(new Handler(
        BasePath + path,
        std::forward<Get>(get),
        std::forward<Put>(put)));
}

template <typename Handler, typename Get, typename Put>
void add(StringView path, Get&& get, Put&& put) {
    add<Handler, Get, Put>(
        path.toString(),
        std::forward<Get>(get),
        std::forward<Put>(put));
}

template <typename WebHandler, typename Handler>
void make_pending(std::forward_list<internal::Pending<Handler>>& pending) {
    // registration order, emplace_front() reversed it
    pending.reverse();
    for (auto& entry : pending) {
        add<WebHandler>(std::move(entry.path), std::move(entry.get), std::move(entry.put));
    }

    pending.clear();
}

void reload() {
    if (apiEnabled()) {
        make_pending<BasicWebHandler>(internal::pending_basic);
        make_pending<JsonWebHandler>(internal::pending_json);
    }
}

void register_basic(String path, BasicHandler&& get, BasicHandler&& put) {
    if (apiEnabled()) {
        add<BasicWebHandler>(std::move(path), std::move(get), std::move(put));
        return;
    }

    internal::pending_basic.emplace_front(
        internal::Pending<BasicHandler>{std::move(path), std::move(get), std::move(put)});
}

void register_json(String path, JsonHandler&& get, JsonHandler&& put) {
    if (apiEnabled()) {
        add<JsonWebHandler>(std::move(path), std::move(get), std::move(put));
        return;
    }

    internal::pending_json.emplace_front(
        internal::Pending<JsonHandler>{std::move(path), std::move(get), std::move(put)});
}

void setup() {
    espurnaRegisterReload(reload);

    register_basic(
        STRING_VIEW("list").toString(),
        [](Request& request) {
            String paths;
            for (auto& api : internal::list) {
                paths += api->pattern();
                paths += '\r';
                paths += '\n';
            }
            request.send(paths);
            return true;
        },
        nullptr
    );

    register_basic(
        STRING_VIEW("rpc").toString(),
        nullptr,
        [](Request& request) {
            STRING_VIEW_INLINE(Action, "action");
            if (rpcHandleAction(request.param(Action.toString()))) {
                return simple::ok(request);
            }
            return simple::error(request);
        }
    );
}

} // namespace
} // namespace api
} // namespace espurna

// -----------------------------------------------------------------------------

void apiRegister(String path, espurna::api::BasicHandler&& get, espurna::api::BasicHandler&& put) {
    espurna::api::register_basic(std::move(path), std::move(get), std::move(put));
}

void apiRegister(String path, espurna::api::JsonHandler&& get, espurna::api::JsonHandler&& put) {
    espurna::api::register_json(std::move(path), std::move(get), std::move(put));
}

void apiSetup() {
    espurna::api::setup();
}


#endif // API_SUPPORT

