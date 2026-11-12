/*

HTTP SERVER - the implementation (libs/http_server.h), part of libs/net_impl.h. No Arduino.

*/

#pragma once

#include "http_server.h"
#include "text.h"
#include "url_parse.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <new>
#include <vector>

// BearSSL: MD5 of the Digest login (the one hash library of ESPurna)
#if __has_include(<bearssl/bearssl.h>)
#include <bearssl/bearssl.h>
#else
#include <bearssl.h>
#endif

namespace espurna {
namespace http {
namespace server {
namespace {

constexpr uint32_t HeadTimeout { 10000 };       // the request line and the headers
constexpr uint32_t IdleTimeout { 30000 };       // nothing moved (body, response, an answer that does not come)
constexpr size_t HeadersMax { 24 };
constexpr size_t PieceSize { 512 };

const char* statusText(int code) {
    switch (code) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    }
    return "";
}

Method methodOf(const char* name) {
    struct Entry {
        const char* name;
        Method method;
    };
    static const Entry Entries[] {
        {"GET", Method::Get}, {"POST", Method::Post}, {"PUT", Method::Put}, {"DELETE", Method::Delete},
        {"PATCH", Method::Patch}, {"HEAD", Method::Head}, {"OPTIONS", Method::Options},
    };
    for (const auto& entry : Entries) {
        if (!strcmp(name, entry.name)) {
            return entry.method;
        }
    }
    return static_cast<Method>(0);
}

int hexDigit(char c) {
    return ((c >= '0') && (c <= '9')) ? (c - '0')
        : ((c >= 'a') && (c <= 'f')) ? (c - 'a' + 10)
        : ((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10)
        : -1;
}

// %XX and '+' of a query / form value, in place
void urlDecode(char* text) {
    char* out = text;
    for (char* in = text; *in; ++in) {
        if ((*in == '%') && (hexDigit(in[1]) >= 0) && (hexDigit(in[2]) >= 0)) {
            *out++ = static_cast<char>((hexDigit(in[1]) << 4) | hexDigit(in[2]));
            in += 2;
        } else if (*in == '+') {
            *out++ = ' ';
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

void md5Hex(br_md5_context& ctx, char* out) {
    uint8_t digest[br_md5_SIZE];
    br_md5_out(&ctx, digest);
    hex(digest, sizeof(digest), out);
}

void md5Add(br_md5_context& ctx, const char* value, bool colon = true) {
    br_md5_update(&ctx, value, strlen(value));
    if (colon) {
        br_md5_update(&ctx, ":", 1);
    }
}

// the nonce of the Digest login: new with every boot
const char* nonce() {
    static char value[33] {};
    if (!value[0]) {
        uint8_t random[16];
        chip::random(random, sizeof(random));
        hex(random, sizeof(random), value);
    }
    return value;
}

struct Route {
    char path[64];
    uint8_t methods;
    Handler handler;
    BodyHandler body;

    bool matches(const char* url, Method method) const {
        if (!(methods & method)) {
            return false;
        }
        const size_t len = strlen(path);
        if (len && (path[len - 1] == '*')) {
            return strncmp(url, path, len - 1) == 0;
        }
        return strcmp(url, path) == 0;
    }
};

struct Rewrite {
    char from[64];
    char to[64];
};

struct DefaultHeader {
    char name[48];
    char value[96];
};

using DefaultHeaders = std::vector<DefaultHeader>;

} // namespace

bool headerParam(const char* header, const char* key, char* out, size_t size) {
    if (!header || !size) {
        return false;
    }

    const size_t key_len = strlen(key);
    for (const char* ptr = header; *ptr; ++ptr) {
        // at the start of a key: after the scheme's space or after a comma
        if ((ptr != header) && (ptr[-1] != ' ') && (ptr[-1] != ',')) {
            continue;
        }
        if (strncasecmp(ptr, key, key_len) || (ptr[key_len] != '=')) {
            continue;
        }

        const char* value = ptr + key_len + 1;
        const bool quoted = (*value == '"');
        if (quoted) {
            ++value;
        }

        size_t len = 0;
        while (value[len] && (quoted ? (value[len] != '"') : ((value[len] != ',') && (value[len] != ' ')))) {
            ++len;
        }

        len = std::min(len, size - 1);
        memcpy(out, value, len);
        out[len] = '\0';
        return true;
    }

    out[0] = '\0';
    return false;
}

// --- a request -------------------------------------------------------------------------------------------------------

class Request::Impl {
public:
    explicit Impl(std::unique_ptr<tcp::Connection> conn) :
        _conn(std::move(conn)),
        _request(*this)
    {
        _started = tcp::now();
        _last = _started;
    }

    ~Impl() {
        if (_context && _deleter) {
            _deleter(_context);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // --- what the handlers see ----------------------------------------------------------------------------------

    Method method() const {
        return _method;
    }

    const char* methodName() const {
        return _method_name;
    }

    const char* url() const {
        return _url;
    }

    const char* query() const {
        return _query ? _query : "";
    }

    const char* header(const char* name) const {
        for (size_t index = 0; index < _headers; ++index) {
            if (!strcasecmp(_header_names[index], name)) {
                return _header_values[index];
            }
        }
        return nullptr;
    }

    const char* param(const char* name) const {
        const char* ptr = _params.data();
        const char* end = ptr + _params.size();
        while (ptr < end) {
            const char* value = ptr + strlen(ptr) + 1;
            if (!strcmp(ptr, name)) {
                return value;
            }
            ptr = value + strlen(value) + 1;
        }
        return nullptr;
    }

    // the n-th name in the list of name and value pairs, nullptr past the end
    const char* paramAt(size_t index, bool value) const {
        const char* ptr = _params.data();
        const char* end = ptr + _params.size();
        size_t current = 0;
        while (ptr < end) {
            const char* val = ptr + strlen(ptr) + 1;
            if (current == index) {
                return value ? val : ptr;
            }
            ptr = val + strlen(val) + 1;
            ++current;
        }
        return nullptr;
    }

    size_t params() const {
        size_t out = 0;
        while (paramAt(out, false)) {
            ++out;
        }
        return out;
    }

    size_t contentLength() const {
        return _content_length;
    }

    size_t bodyReceived() const {
        return _body_received;
    }

    size_t fileSize() const {
        if (_multipart == Multipart::None) {
            return _content_length;
        }

        // the closing delimiter: CR LF -- boundary -- CR LF
        const size_t tail = strlen(_delimiter) + 4;
        return (_content_length > _part_offset + tail) ? (_content_length - _part_offset - tail) : 0;
    }

    const char* contentType() const {
        const char* out = header("Content-Type");
        return out ? out : "";
    }

    uint32_t remoteIp() const {
        return _conn ? _conn->remoteIp() : 0;
    }

    uint32_t localIp() const {
        return _conn ? _conn->localIp() : 0;
    }

    bool authenticate(const char* user, const char* password, const char* realm) const {
        const char* auth = header("Authorization");
        if (!auth) {
            return false;
        }

        if (!strncasecmp(auth, "Basic ", 6)) {
            std::vector<uint8_t> plain;
            if (!text::base64Decode(plain, auth + 6, strlen(auth + 6))) {
                return false;
            }
            const size_t user_len = strlen(user);
            const size_t pass_len = strlen(password);
            return (plain.size() == user_len + 1 + pass_len)
                && !memcmp(plain.data(), user, user_len) && (plain[user_len] == ':')
                && !memcmp(plain.data() + user_len + 1, password, pass_len);
        }

        if (strncasecmp(auth, "Digest ", 7)) {
            return false;
        }

        char value[96];
        if (!headerParam(auth, "username", value, sizeof(value)) || strcmp(value, user)) {
            return false;
        }

        // the response is checked with the nonce the client used: right with another one (of another boot, of the
        // full image before the mini) is stale, not a wrong password
        char client_nonce[48];
        if (!headerParam(auth, "nonce", client_nonce, sizeof(client_nonce))) {
            return false;
        }

        char uri[UrlMax];
        if (!headerParam(auth, "uri", uri, sizeof(uri))) {
            return false;
        }

        char ha1[33];
        char ha2[33];
        br_md5_context ctx;
        br_md5_init(&ctx);
        md5Add(ctx, user);
        md5Add(ctx, realm);
        md5Add(ctx, password, false);
        md5Hex(ctx, ha1);

        br_md5_init(&ctx);
        md5Add(ctx, _method_name);
        md5Add(ctx, uri, false);
        md5Hex(ctx, ha2);

        br_md5_init(&ctx);
        md5Add(ctx, ha1);
        md5Add(ctx, client_nonce);
        char qop[16];
        if (headerParam(auth, "qop", qop, sizeof(qop)) && qop[0]) {
            headerParam(auth, "nc", value, sizeof(value));
            md5Add(ctx, value);
            headerParam(auth, "cnonce", value, sizeof(value));
            md5Add(ctx, value);
            md5Add(ctx, qop);
        }
        md5Add(ctx, ha2, false);

        char expected[33];
        md5Hex(ctx, expected);
        const bool right = headerParam(auth, "response", value, sizeof(value)) && !strcasecmp(value, expected);
        if (right && strcmp(client_nonce, nonce())) {
            _stale = true;
            return false;
        }

        return right;
    }

    void requestAuthentication(const char* realm) {
        char challenge[192];
        snprintf(challenge, sizeof(challenge), "Digest realm=\"%s\", qop=\"auth\", nonce=\"%s\", opaque=\"%s\"%s",
            realm, nonce(), nonce(), _stale ? ", stale=TRUE" : "");
        addHeader("WWW-Authenticate", challenge);
        send(401, "text/plain", "Authentication required");
    }

    bool addHeader(const char* name, const char* value) {
        const size_t need = strlen(name) + strlen(value) + 4;
        if (_extra_len + need >= sizeof(_extra)) {
            return false;
        }
        _extra_len += snprintf(_extra + _extra_len, sizeof(_extra) - _extra_len, "%s: %s\r\n", name, value);
        return true;
    }

    void send(int code, const char* type, const uint8_t* body, size_t len) {
        if (_answered) {
            return;
        }

        _body_copy.reset();
        _body_len = 0;
        if (len) {
            _body_copy.reset(new (std::nothrow) uint8_t[len]);
            if (_body_copy) {
                memcpy(_body_copy.get(), body, len);
                _body_len = len;
            } else {
                code = 500;
            }
        }

        respond(code, type, static_cast<long>(_body_len), nullptr);
    }

    void send(int code, const char* type, long length, Filler filler) {
        if (_answered) {
            return;
        }
        respond(code, type, length, std::move(filler));
    }

    bool answered() const {
        return _answered;
    }

    std::unique_ptr<tcp::Connection> takeConnection() {
        _taken = true;
        _answered = true;
        return std::move(_conn);
    }

    void setContext(void* context, void (*deleter)(void*)) {
        if (_context && _deleter) {
            _deleter(_context);
        }
        _context = context;
        _deleter = deleter;
    }

    void* context() const {
        return _context;
    }

    void onDisconnect(std::function<void()> callback) {
        _on_disconnect = std::move(callback);
    }

    // --- the server's side ------------------------------------------------------------------------------------------

    bool done() const {
        return _state == State::Done;
    }

    // one step, never waiting
    void step(Server::Impl& server);

    Request& request() {
        return _request;
    }

private:
    enum class State {
        Head,
        Body,
        Answer,     // the handler ran, its answer may come later
        Respond,
        Done,
    };

    enum class Multipart {
        None,
        Preamble,
        Headers,
        Data,
        End,
    };

    void respond(int code, const char* type, long length, Filler filler) {
        _answered = true;
        _filler = std::move(filler);
        _chunked = (length < 0);
        _code = code;

        int len = snprintf(_response, sizeof(_response),
            "HTTP/1.1 %d %s\r\nConnection: close\r\n", code, statusText(code));
        if (type && type[0] && (len > 0)) {
            len += snprintf(_response + len, sizeof(_response) - len, "Content-Type: %s\r\n", type);
        }
        if (len > 0) {
            if (_chunked) {
                len += snprintf(_response + len, sizeof(_response) - len, "Transfer-Encoding: chunked\r\n");
            } else {
                len += snprintf(_response + len, sizeof(_response) - len, "Content-Length: %ld\r\n", length);
            }
        }
        if (_defaults) {
            for (const auto& header : *_defaults) {
                if ((len > 0) && (static_cast<size_t>(len) < sizeof(_response))) {
                    len += snprintf(_response + len, sizeof(_response) - len, "%s: %s\r\n", header.name, header.value);
                }
            }
        }
        if ((len > 0) && (static_cast<size_t>(len) + _extra_len + 2 < sizeof(_response))) {
            memcpy(_response + len, _extra, _extra_len);
            len += _extra_len;
            _response[len++] = '\r';
            _response[len++] = '\n';
        }

        _response_len = (len > 0) ? std::min<size_t>(len, sizeof(_response)) : 0;
        _response_sent = 0;
        _body_index = 0;
        _body_sent = 0;
        _piece_len = 0;
        _piece_sent = 0;
        _finished = false;
    }

    // --- the head ---------------------------------------------------------------------------------------------------

    // the head grows as it comes (512, 1024 ... HeadMax): a short request costs little, a browser with its login and
    // the cookies of the address (a few KB) still fits
    bool readHead() {
        for (;;) {
            if (_head_len + 1 >= _head_size) {
                if (_head_size >= HeadMax) {
                    break;
                }
                const size_t size = _head_size ? std::min(_head_size * 2, HeadMax) : 512;
                std::unique_ptr<char[]> head(new (std::nothrow) char[size]);
                if (!head) {
                    break;
                }
                if (_head_len) {
                    memcpy(head.get(), _head, _head_len);
                }
                _head_buffer = std::move(head);
                _head = _head_buffer.get();
                _head_size = size;
            }

            uint8_t c;
            if (!_conn->read(&c, 1)) {
                return false;
            }
            _head[_head_len++] = static_cast<char>(c);
            if ((_head_len >= 4) && !memcmp(_head + _head_len - 4, "\r\n\r\n", 4)) {
                _head[_head_len] = '\0';
                return true;
            }
        }

        _head_overflow = true;
        return true;
    }

    void addParams(char* text) {
        while (text && *text) {
            char* next = strchr(text, '&');
            if (next) {
                *next++ = '\0';
            }

            char* value = strchr(text, '=');
            if (value) {
                *value++ = '\0';
            } else {
                value = text + strlen(text);
            }

            urlDecode(text);
            urlDecode(value);
            addParam(text, value);
            text = next;
        }
    }

    void addParam(const char* name, const char* value) {
        _params.insert(_params.end(), name, name + strlen(name) + 1);
        _params.insert(_params.end(), value, value + strlen(value) + 1);
    }

    bool parseHead() {
        char* line = _head;
        char* end = strstr(line, "\r\n");
        if (!end) {
            return false;
        }
        *end = '\0';

        char* space = strchr(line, ' ');
        if (!space) {
            return false;
        }
        *space = '\0';
        copy(_method_name, sizeof(_method_name), line);
        _method = methodOf(_method_name);

        char* target = space + 1;
        space = strchr(target, ' ');
        if (!space) {
            return false;
        }
        *space = '\0';

        char* query = strchr(target, '?');
        if (query) {
            *query++ = '\0';
        }
        copy(_url, sizeof(_url), target);
        _query = query;
        if (query) {
            // the query stays readable as it came: its parameters are taken from a copy
            const size_t len = strlen(query);
            std::vector<char> copy_of(query, query + len + 1);
            addParams(copy_of.data());
        }

        _headers = 0;
        line = end + 2;
        while ((line < _head + _head_len) && *line) {
            end = strstr(line, "\r\n");
            if (!end) {
                break;
            }
            *end = '\0';
            if (!*line) {
                break;
            }

            char* colon = strchr(line, ':');
            if (colon && (_headers < HeadersMax)) {
                *colon = '\0';
                char* value = colon + 1;
                while (*value == ' ') {
                    ++value;
                }
                _header_names[_headers] = line;
                _header_values[_headers] = value;
                ++_headers;
            }
            line = end + 2;
        }

        const char* length = header("Content-Length");
        _content_length = length ? static_cast<size_t>(strtoul(length, nullptr, 10)) : 0;
        return _method != static_cast<Method>(0);
    }

    // --- the body -----------------------------------------------------------------------------------------------

    bool form() const {
        return !strncasecmp(contentType(), "application/x-www-form-urlencoded", 33)
            && (_content_length <= FormMax);
    }

    void startBody() {
        _body_received = 0;
        _multipart = Multipart::None;
        if (!_content_length || form() || !_route || !_route->body) {
            return;
        }

        const char* type = contentType();
        if (!strncasecmp(type, "multipart/form-data", 19)) {
            char boundary[72];
            if (headerParam(type, "boundary", boundary, sizeof(boundary)) && boundary[0]) {
                // the delimiter of every part after the first: CR LF -- boundary
                snprintf(_delimiter, sizeof(_delimiter), "\r\n--%s", boundary);
                _multipart = Multipart::Preamble;
                // the first one comes without the CR LF: as if it had them
                _scan.assign({'\r', '\n'});
                _scan_in = 2;
            }
        }
    }

    // a piece of the body as it came
    void bodyPiece(const uint8_t* data, size_t len) {
        if (form()) {
            _form.insert(_form.end(), data, data + len);
            return;
        }

        if (!_route || !_route->body) {
            return;
        }

        if (_multipart == Multipart::None) {
            const bool final = (_body_received == _content_length);
            _route->body(_request, "", _body_received - len, data, len, final);
            return;
        }

        _scan.insert(_scan.end(), data, data + len);
        _scan_in += len;
        multipart();
    }

    // the file part of a multipart body: what lies between the part headers and the next delimiter
    void multipart() {
        const size_t delim_len = strlen(_delimiter);
        for (;;) {
            if ((_multipart == Multipart::Preamble) || (_multipart == Multipart::Data)) {
                const auto* begin = _scan.data();
                const auto* found = static_cast<const uint8_t*>(nullptr);
                for (size_t at = 0; at + delim_len <= _scan.size(); ++at) {
                    if (!memcmp(begin + at, _delimiter, delim_len)) {
                        found = begin + at;
                        break;
                    }
                }

                if (!found) {
                    // keep what could be the start of the delimiter
                    if (_scan.size() > delim_len) {
                        const size_t take = _scan.size() - delim_len;
                        if (_multipart == Multipart::Data) {
                            fileData(begin, take, false);
                        }
                        _scan.erase(_scan.begin(), _scan.begin() + take);
                    }
                    return;
                }

                const size_t take = found - begin;
                if (_multipart == Multipart::Data) {
                    fileData(begin, take, true);
                }
                _scan.erase(_scan.begin(), _scan.begin() + take + delim_len);
                _multipart = Multipart::Headers;
                _part_name[0] = '\0';
                _part_file = false;
                continue;
            }

            if (_multipart == Multipart::Headers) {
                // "--" after the delimiter: the end; else CR LF, the part headers, an empty line
                if (_scan.size() < 2) {
                    return;
                }
                if ((_scan[0] == '-') && (_scan[1] == '-')) {
                    _multipart = Multipart::End;
                    _scan.clear();
                    return;
                }

                const char* text = reinterpret_cast<const char*>(_scan.data());
                const size_t size = _scan.size();
                size_t end = 0;
                while ((end + 4 <= size) && memcmp(text + end, "\r\n\r\n", 4)) {
                    ++end;
                }
                if (end + 4 > size) {
                    if (size > HeadMax) {
                        _multipart = Multipart::End;
                        _scan.clear();
                    }
                    return;
                }

                std::vector<char> headers(text, text + end);
                headers.push_back('\0');
                const char* disposition = nullptr;
                for (const char* ptr = headers.data(); *ptr; ++ptr) {
                    if (!strncasecmp(ptr, "Content-Disposition:", 20)) {
                        disposition = ptr;
                        break;
                    }
                }
                if (disposition) {
                    headerParam(disposition, "name", _part_name, sizeof(_part_name));
                    char file[64];
                    _part_file = headerParam(disposition, "filename", file, sizeof(file));
                    if (_part_file) {
                        copy(_part_filename, sizeof(_part_filename), file);
                    }
                }

                _scan.erase(_scan.begin(), _scan.begin() + end + 4);
                // where the data of this part starts in the body (the 2 bytes put in front not counted)
                _part_offset = _scan_in - 2 - _scan.size();
                _part_index = 0;
                _part_value.clear();
                _multipart = Multipart::Data;
                continue;
            }

            return;
        }
    }

    // a file part to the body handler; a small field part becomes a parameter
    void fileData(const uint8_t* data, size_t len, bool final) {
        if (_part_file) {
            if (len || final) {
                _route->body(_request, _part_filename, _part_index, data, len, final);
            }
            _part_index += len;
            return;
        }

        if (_part_value.size() + len <= FormMax) {
            _part_value.insert(_part_value.end(), data, data + len);
        }
        if (final && _part_name[0]) {
            _part_value.push_back('\0');
            addParam(_part_name, _part_value.data());
        }
    }

    // --- the response -------------------------------------------------------------------------------------------

    // false - nothing more can go now
    bool sendBody() {
        if (_finished) {
            return false;
        }

        if (!_piece_len || (_piece_sent == _piece_len)) {
            _piece_len = 0;
            _piece_sent = 0;
            if (_body_copy) {
                const size_t take = std::min(PieceSize, _body_len - _body_index);
                if (!take) {
                    _finished = true;
                    return false;
                }
                memcpy(_piece + (_chunked ? 6 : 0), _body_copy.get() + _body_index, take);
                frame(take);
                _body_index += take;
            } else if (_filler) {
                const size_t take = _filler(_piece + (_chunked ? 6 : 0), PieceSize, _body_index);
                if (!take) {
                    _finished = true;
                    if (_chunked) {
                        memcpy(_piece, "0\r\n\r\n", 5);
                        _piece_len = 5;
                        _piece_sent = 0;
                        return true;
                    }
                    return false;
                }
                frame(take);
                _body_index += take;
            } else {
                _finished = true;
                return false;
            }
        }

        return true;
    }

    // the chunk size in front of the data (6 bytes reserved) and CR LF after it
    void frame(size_t take) {
        if (!_chunked) {
            _piece_len = take;
            return;
        }

        char size[8];
        const int len = snprintf(size, sizeof(size), "%x\r\n", static_cast<unsigned>(take));
        const size_t offset = 6 - len;
        memcpy(_piece + offset, size, len);
        _piece[6 + take] = '\r';
        _piece[6 + take + 1] = '\n';
        _piece_start = offset;
        _piece_len = 6 + take + 2;
        _piece_sent = offset;
    }

    void stepRespond() {
        while (_response_sent < _response_len) {
            const auto sent = _conn->write(reinterpret_cast<const uint8_t*>(_response) + _response_sent,
                _response_len - _response_sent);
            if (!sent) {
                return;
            }
            _response_sent += sent;
            _last = tcp::now();
        }

        if (_method == Method::Head) {
            finish();
            return;
        }

        for (;;) {
            if (_piece_sent < _piece_len) {
                const auto sent = _conn->write(_piece + _piece_sent, _piece_len - _piece_sent);
                if (!sent) {
                    return;
                }
                _piece_sent += sent;
                _last = tcp::now();
                continue;
            }

            if (!sendBody() && (_piece_sent >= _piece_len)) {
                finish();
                return;
            }
        }
    }

    void finish() {
        if (_conn) {
            _conn->close();
        }
        _state = State::Done;
    }

    void fail(int code) {
        _extra_len = 0;
        send(code, "text/plain", reinterpret_cast<const uint8_t*>(statusText(code)), strlen(statusText(code)));
        _state = State::Respond;
    }

    friend class Server::Impl;

    std::unique_ptr<tcp::Connection> _conn;
    Request _request;
    const DefaultHeaders* _defaults { nullptr };
    State _state { State::Head };
    uint32_t _started { 0 };
    uint32_t _last { 0 };
    bool _taken { false };

    std::unique_ptr<char[]> _head_buffer;
    char* _head { nullptr };
    size_t _head_size { 0 };
    size_t _head_len { 0 };
    bool _head_overflow { false };
    Method _method { static_cast<Method>(0) };
    char _method_name[8] {};
    char _url[UrlMax] {};
    char* _query { nullptr };
    const char* _header_names[HeadersMax] {};
    const char* _header_values[HeadersMax] {};
    size_t _headers { 0 };
    std::vector<char> _params;

    const Route* _route { nullptr };
    size_t _content_length { 0 };
    size_t _body_received { 0 };
    std::vector<uint8_t> _form;
    Multipart _multipart { Multipart::None };
    char _delimiter[80] {};
    std::vector<uint8_t> _scan;
    size_t _scan_in { 0 };
    size_t _part_offset { 0 };
    char _part_name[32] {};
    char _part_filename[64] {};
    bool _part_file { false };
    size_t _part_index { 0 };
    std::vector<char> _part_value;

    void* _context { nullptr };
    void (*_deleter)(void*) { nullptr };
    std::function<void()> _on_disconnect;

    bool _answered { false };
    mutable bool _stale { false };
    int _code { 0 };
    char _extra[256] {};
    size_t _extra_len { 0 };
    char _response[ResponseHeadMax] {};
    size_t _response_len { 0 };
    size_t _response_sent { 0 };
    std::unique_ptr<uint8_t[]> _body_copy;
    size_t _body_len { 0 };
    Filler _filler;
    bool _chunked { false };
    size_t _body_index { 0 };
    size_t _body_sent { 0 };
    uint8_t _piece[6 + PieceSize + 2] {};
    size_t _piece_start { 0 };
    size_t _piece_len { 0 };
    size_t _piece_sent { 0 };
    bool _finished { false };
};

// --- the server --------------------------------------------------------------------------------------------------------

class Server::Impl {
public:
    static constexpr size_t RequestsMax { 4 };

    bool begin(uint16_t port) {
        return _listener.begin(port, RequestsMax);
    }

    void end() {
        _listener.end();
        _requests.clear();
    }

    void on(const char* path, uint8_t methods, Handler handler, BodyHandler body) {
        Route route;
        copy(route.path, sizeof(route.path), path);
        route.methods = methods;
        route.handler = std::move(handler);
        route.body = std::move(body);
        _routes.push_back(std::move(route));
    }

    void rewrite(const char* from, const char* to) {
        Rewrite out;
        copy(out.from, sizeof(out.from), from);
        copy(out.to, sizeof(out.to), to);
        _rewrites.push_back(out);
    }

    void onNotFound(Handler handler, BodyHandler body) {
        copy(_fallback.path, sizeof(_fallback.path), "*");
        _fallback.methods = Method::Any;
        _fallback.handler = std::move(handler);
        _fallback.body = std::move(body);
    }

    void onAccess(Handler handler) {
        _access = std::move(handler);
    }

    void access(Request& request) const {
        if (_access) {
            _access(request);
        }
    }

    void onUpgrade(Handler handler) {
        _upgrade = std::move(handler);
    }

    void defaultHeader(const char* name, const char* value) {
        for (auto& header : _defaults) {
            if (!strcasecmp(header.name, name)) {
                copy(header.value, sizeof(header.value), value);
                return;
            }
        }

        DefaultHeader header;
        copy(header.name, sizeof(header.name), name);
        copy(header.value, sizeof(header.value), value);
        _defaults.push_back(header);
    }

    size_t active() const {
        return _requests.size();
    }

    void loop() {
        while (_requests.size() < RequestsMax) {
            auto conn = _listener.accept();
            if (!conn) {
                break;
            }

            std::unique_ptr<Request::Impl> request(new (std::nothrow) Request::Impl(std::move(conn)));
            if (request) {
                request->_defaults = &_defaults;
                _requests.push_back(std::move(request));
            }
        }

        for (auto& request : _requests) {
            request->step(*this);
        }

        _requests.erase(std::remove_if(_requests.begin(), _requests.end(),
            [](const std::unique_ptr<Request::Impl>& request) {
                return request->done();
            }), _requests.end());
    }

    const Route* route(char* url, Method method) const {
        for (const auto& rewrite : _rewrites) {
            if (!strcmp(url, rewrite.from)) {
                copy(url, UrlMax, rewrite.to);
                break;
            }
        }

        for (const auto& route : _routes) {
            if (route.matches(url, method)) {
                return &route;
            }
        }

        return _fallback.handler ? &_fallback : nullptr;
    }

    void notFound(Request& request) const {
        request.send(404, "text/plain", "Not Found");
    }

    bool upgrade(Request& request) const {
        if (!_upgrade) {
            return false;
        }
        _upgrade(request);
        return true;
    }

private:
    tcp::Listener _listener;
    std::vector<Route> _routes;
    std::vector<Rewrite> _rewrites;
    Route _fallback {};
    Handler _access;
    Handler _upgrade;
    DefaultHeaders _defaults;
    std::vector<std::unique_ptr<Request::Impl>> _requests;
};

void Request::Impl::step(Server::Impl& server) {
    if (_state == State::Done) {
        return;
    }

    const auto now = tcp::now();
    // gone: closed, and nothing of the request is left to read (a client may close its side after sending)
    if (!_conn || (_conn->closed() && !_conn->available() && (_state != State::Respond))) {
        if (_on_disconnect && !_taken) {
            auto callback = std::move(_on_disconnect);
            _on_disconnect = nullptr;
            callback();
        }
        finish();
        return;
    }

    switch (_state) {
    case State::Head:
        if (now - _started > HeadTimeout) {
            finish();
            return;
        }
        if (!readHead()) {
            return;
        }
        if (_head_overflow) {
            fail(431);
            return;
        }
        if (!parseHead()) {
            fail(400);
            return;
        }

        _last = now;
        _route = server.route(_url, _method);
        server.access(_request);

        {
            const char* upgrade = header("Upgrade");
            if (upgrade && !strcasecmp(upgrade, "websocket")) {
                if (!server.upgrade(_request)) {
                    fail(501);
                    return;
                }
                if (_taken) {
                    _state = State::Done;
                    return;
                }
                _state = _answered ? State::Respond : State::Answer;
                return;
            }
        }

        startBody();
        _state = State::Body;
        // a body may be in already
        /* fall through */

    case State::Body: {
        uint8_t buffer[PieceSize];
        while (_body_received < _content_length) {
            const auto got = _conn->read(buffer, std::min(sizeof(buffer), _content_length - _body_received));
            if (!got) {
                if (now - _last > IdleTimeout) {
                    finish();
                }
                return;
            }
            _last = now;
            _body_received += got;
            bodyPiece(buffer, got);
        }

        if (form() && !_form.empty()) {
            _form.push_back('\0');
            addParams(reinterpret_cast<char*>(_form.data()));
            _form.clear();
        }

        if (_route) {
            _route->handler(_request);
        } else {
            server.notFound(_request);
        }

        _state = _answered ? State::Respond : State::Answer;
        _last = now;
        return;
    }

    case State::Answer:
        if (_answered) {
            _state = State::Respond;
        } else if (now - _last > IdleTimeout) {
            fail(503);
        }
        return;

    case State::Respond:
        if (_conn->closed()) {
            finish();
            return;
        }
        stepRespond();
        if ((_state != State::Done) && (now - _last > IdleTimeout)) {
            finish();
        }
        return;

    case State::Done:
        return;
    }
}

// --- the forwarders ----------------------------------------------------------------------------------------------------

Method Request::method() const { return _impl.method(); }
const char* Request::methodName() const { return _impl.methodName(); }
const char* Request::url() const { return _impl.url(); }
const char* Request::query() const { return _impl.query(); }
const char* Request::header(const char* name) const { return _impl.header(name); }
const char* Request::param(const char* name) const { return _impl.param(name); }
size_t Request::params() const { return _impl.params(); }
const char* Request::paramName(size_t index) const { return _impl.paramAt(index, false); }
const char* Request::paramValue(size_t index) const { return _impl.paramAt(index, true); }
size_t Request::contentLength() const { return _impl.contentLength(); }
size_t Request::bodyReceived() const { return _impl.bodyReceived(); }
size_t Request::fileSize() const { return _impl.fileSize(); }
const char* Request::contentType() const { return _impl.contentType(); }
uint32_t Request::remoteIp() const { return _impl.remoteIp(); }
uint32_t Request::localIp() const { return _impl.localIp(); }

bool Request::authenticate(const char* user, const char* password, const char* realm) const {
    return _impl.authenticate(user, password, realm);
}

void Request::requestAuthentication(const char* realm) { _impl.requestAuthentication(realm); }
bool Request::addHeader(const char* name, const char* value) { return _impl.addHeader(name, value); }

void Request::send(int code, const char* type, const char* body) {
    _impl.send(code, type, reinterpret_cast<const uint8_t*>(body), body ? strlen(body) : 0);
}

void Request::send(int code, const char* type, const uint8_t* body, size_t len) { _impl.send(code, type, body, len); }
void Request::send(int code, const char* type, long length, Filler filler) {
    _impl.send(code, type, length, std::move(filler));
}

bool Request::answered() const { return _impl.answered(); }
std::unique_ptr<tcp::Connection> Request::takeConnection() { return _impl.takeConnection(); }
void Request::setContext(void* context, void (*deleter)(void*)) { _impl.setContext(context, deleter); }
void* Request::context() const { return _impl.context(); }
void Request::onDisconnect(std::function<void()> callback) { _impl.onDisconnect(std::move(callback)); }

Server::Server() :
    _impl(new (std::nothrow) Impl())
{}

Server::~Server() = default;

bool Server::begin(uint16_t port) { return _impl && _impl->begin(port); }
void Server::end() { if (_impl) { _impl->end(); } }

void Server::on(const char* path, uint8_t methods, Handler handler, BodyHandler body) {
    if (_impl) {
        _impl->on(path, methods, std::move(handler), std::move(body));
    }
}

void Server::rewrite(const char* from, const char* to) { if (_impl) { _impl->rewrite(from, to); } }
void Server::onNotFound(Handler handler, BodyHandler body) {
    if (_impl) {
        _impl->onNotFound(std::move(handler), std::move(body));
    }
}

void Server::onAccess(Handler handler) { if (_impl) { _impl->onAccess(std::move(handler)); } }
void Server::onUpgrade(Handler handler) { if (_impl) { _impl->onUpgrade(std::move(handler)); } }
void Server::defaultHeader(const char* name, const char* value) { if (_impl) { _impl->defaultHeader(name, value); } }
void Server::loop() { if (_impl) { _impl->loop(); } }
size_t Server::active() const { return _impl ? _impl->active() : 0; }

} // namespace server
} // namespace http
} // namespace espurna
