/*

HTTP SERVER - the one web server of everything ESPurna builds: the head of every chip (the WebUI module, the API, the
head page of the modules, /upgrade, /safeboot ...), code/safeboot-app and code/mini-app. No Arduino: on the TCP
connection of libs/tcp.h (lwIP's raw API), the loop drives it, nothing waits; handlers run in the loop.

- a request: the method, the path, the query, the headers (up to HeadMax bytes of them), the parameters of the query
  and of a form body (application/x-www-form-urlencoded, up to FormMax bytes)
- a body that is not a small form goes to the route's body handler a piece at a time: raw, or the file part of a
  multipart/form-data upload (a browser form), so an image of a few MB never sits in RAM
- a response: a status, a type, headers, the body copied (small) or produced by a filler a piece at a time (a page
  from the flash, a long JSON), Content-Length or chunked; one request per connection (Connection: close)
- login: Basic and Digest (MD5, what the browsers take) against a user and a password
- WebSocket: a request with `Upgrade: websocket` goes to the upgrade handler (ESPurna's ws module), which may take the
  TCP connection over (takeConnection()) and speaks the WebSocket protocol on it itself

    espurna::http::server::Server server;
    server.on("/upgrade", Method::Post, [](Request& r) { ... r.send(200, "text/plain", "OK"); }, upload);
    server.begin(80);
    ... server.loop() from the loop ...

The implementation is libs/http_server_impl.h, part of libs/net_impl.h.

*/

#pragma once

#include "tcp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace espurna {
namespace http {
namespace server {

enum Method : uint8_t {
    Get = 1 << 0,
    Post = 1 << 1,
    Put = 1 << 2,
    Delete = 1 << 3,
    Patch = 1 << 4,
    Head = 1 << 5,
    Options = 1 << 6,
    Any = 0x7f,
};

constexpr size_t HeadMax { 4096 };      // the request line and the headers (the buffer grows up to it)
constexpr size_t FormMax { 1024 };      // a form body kept for its parameters
constexpr size_t ResponseHeadMax { 512 };

// A piece of the body: the bytes of the file part of a multipart upload, or of the raw body. `index` is the offset of
// `data` in the body (the file), `final` the last piece. `name` the file name of a multipart upload, else ""
class Request;
using Handler = std::function<void(Request&)>;
using BodyHandler = std::function<void(Request&, const char* name, size_t index, const uint8_t* data, size_t len,
    bool final)>;

// produces the next piece of a response body into `out` (at most `max` bytes) at `index`; 0 - the end
using Filler = std::function<size_t(uint8_t* out, size_t max, size_t index)>;

class Request {
public:
    Method method() const;
    const char* methodName() const;
    const char* url() const;            // the path, without the query
    const char* query() const;          // after '?', "" when none

    // a header of the request, nullptr when it did not come (or did not fit HeadMax)
    const char* header(const char* name) const;

    // a parameter of the query or of the form body, decoded; nullptr when none
    const char* param(const char* name) const;
    bool hasParam(const char* name) const {
        return param(name) != nullptr;
    }

    // every parameter, in the order they came
    size_t params() const;
    const char* paramName(size_t index) const;
    const char* paramValue(size_t index) const;

    size_t contentLength() const;       // what the client said, 0 when nothing
    size_t bodyReceived() const;        // what did arrive of it
    // the size of what the body handler gets: the file of a multipart body (the body less the part head and the
    // closing delimiter, a form with the file as its last part), else the body
    size_t fileSize() const;
    const char* contentType() const;
    uint32_t remoteIp() const;          // IPv4, network order
    uint32_t localIp() const;           // the board's address the request came to

    // login: Basic or Digest (realm = `realm`) with this user and password
    bool authenticate(const char* user, const char* password, const char* realm) const;
    // 401 with the Digest challenge of `realm`; stale=TRUE when authenticate() saw the right password with a nonce
    // of another boot (the browser repeats the request with the new one, without asking)
    void requestAuthentication(const char* realm);

    // the response; one per request. Extra headers go before it
    bool addHeader(const char* name, const char* value);
    void send(int code, const char* type, const char* body);
    void send(int code, const char* type, const uint8_t* body, size_t len);
    // `length` < 0: chunked
    void send(int code, const char* type, long length, Filler filler);
    bool answered() const;

    // the upgrade handler: the TCP connection, out of the server's hands (nothing is answered by the server then)
    std::unique_ptr<tcp::Connection> takeConnection();

    // the handler's own state of this request (freed with the request by its deleter)
    void setContext(void* context, void (*deleter)(void*));
    void* context() const;

    // the client went away before the response was out (the request is dropped right after)
    void onDisconnect(std::function<void()> callback);

    class Impl;
    explicit Request(Impl& impl) :
        _impl(impl)
    {}

private:
    Impl& _impl;
};

class Server {
public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool begin(uint16_t port);
    void end();

    // `path` exact, or a prefix when it ends with '*'. The body handler takes a body that is not a small form;
    // `handler` runs once the whole body is in
    void on(const char* path, uint8_t methods, Handler handler, BodyHandler body = nullptr);
    // a path answered as another one (the ESP32's /upgrade on the safeboot layout)
    void rewrite(const char* from, const char* to);
    // the requests no route took, with their body
    void onNotFound(Handler handler, BodyHandler body = nullptr);
    // every request, once its head is in (the access log)
    void onAccess(Handler handler);
    // `Upgrade: websocket` requests
    void onUpgrade(Handler handler);
    // a header of every response (CORS); replaces one of the same name
    void defaultHeader(const char* name, const char* value);

    // from the loop
    void loop();

    // requests in progress
    size_t active() const;

    class Impl;

private:
    std::unique_ptr<Impl> _impl;
};

// --- login (also of code/mini-app's own use) -------------------------------------------------------------------------

// the value of `key` in a header like Digest's (key=value, key="value"), into `out`; false - none
bool headerParam(const char* header, const char* key, char* out, size_t size);

} // namespace server
} // namespace http
} // namespace espurna
