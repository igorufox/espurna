/*

IMAGE RECEIVER - the one code that takes a firmware image in, everything ESPurna builds: the head of every chip
(/upgrade: ota_web.cpp) and code/migration-app (/upgrade, the download of the full image). No Arduino, header only.

- the image comes a piece at a time: the body of an upload (libs/http_server.h) or the body of a download
  (libs/http_client.h, the receiver is its sink: 200 and Content-Length, the SHA-256 of the release list is checked
  by the client)
- the first FirstPiece bytes (or the whole file when it is shorter) go to the target at once, with the size (0 - not
  known: a form upload): the target checks the header and refuses what this board does not take before anything is
  written. Gathered here, as the pieces of an upload are of any size (the Beken checks a whole UF2 block of 512 bytes;
  the piece after the form part's headers was shorter and every upload was refused, W30)
- more bytes than announced, or fewer at the end, fail it; finish() hands the end to the target, abort() whatever
  happened (the client went away)
- the POSTs that change the firmware are refused for a page of another site (sameOrigin())

What differs is only where the bytes go, the Target of the image:
- the head: the image into the chip (ota.cpp, ota_driver.h, arch/<chip>/ota_<chip>.cpp)
- the migration image: the free space behind the running image and eboot, read back before eboot is told (migration-app)

*/

#pragma once

#include "http_client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace espurna {
namespace image {

// where the image goes; every call: nullptr, or the error (a string that lives on)
struct Target {
    // the first piece, before anything is written: its header decides. `size` of the whole file, 0 - not known
    virtual const char* begin(const uint8_t* data, size_t len, size_t size) = 0;
    virtual const char* write(const uint8_t* data, size_t len) = 0;
    // all of it arrived: check and commit (the next boot runs it)
    virtual const char* end(size_t size) = 0;
    // stopped before the end
    virtual void abort() = 0;

protected:
    ~Target() = default;
};

class Receiver : public http::Sink {
public:
    explicit Receiver(Target& target) :
        _target(target)
    {}

    ~Receiver() {
        abort();
    }

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // the size of the file when known in advance (an upload of a raw body, a download), 0 - not known
    void expect(size_t size) {
        _expected = size;
    }

    // a piece of the image, in order
    const char* push(const uint8_t* data, size_t len) {
        if (_error) {
            return _error;
        }

        if (!len) {
            return nullptr;
        }

        if (_expected && (_received + len > _expected)) {
            return fail("more data than announced");
        }

        if (!_started) {
            const size_t take = std::min(len, FirstPiece - _first_len);
            std::memcpy(&_first[_first_len], data, take);
            _first_len += take;
            _received += take;
            data += take;
            len -= take;
            if ((_first_len < FirstPiece) && !(_expected && (_received == _expected))) {
                return nullptr;
            }

            const char* error = start();
            if (error) {
                return error;
            }
        }

        if (len) {
            const char* error = _target.write(data, len);
            if (error) {
                return fail(error);
            }
            _received += len;
        }

        return nullptr;
    }

    // everything was pushed
    const char* finish() {
        if (_error) {
            return _error;
        }

        if (!_started) {
            if (!_first_len) {
                return fail("no image");
            }
            const char* error = start();
            if (error) {
                return error;
            }
        }

        if (_expected && (_received != _expected)) {
            snprintf(_text, sizeof(_text), "the image is incomplete, %u of %u bytes",
                static_cast<unsigned>(_received), static_cast<unsigned>(_expected));
            return fail(_text);
        }

        _started = false;
        const char* error = _target.end(_received);
        if (error) {
            _error = error;
            return error;
        }

        _done = true;
        return nullptr;
    }

    void abort() {
        if (_started) {
            _started = false;
            _target.abort();
        }
    }

    size_t received() const {
        return _received;
    }

    bool done() const {
        return _done;
    }

    const char* error() const {
        return _error;
    }

    // --- the sink of a download (libs/http_client.h) -----------------------------------------------------------

    const char* begin(const http::Head& head) override {
        if (head.status != 200) {
            snprintf(_text, sizeof(_text), "HTTP %d", head.status);
            return fail(_text);
        }

        if (head.length <= 0) {
            return fail("no Content-Length in the response");
        }

        expect(static_cast<size_t>(head.length));
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        return push(data, len);
    }

    const char* end() override {
        return finish();
    }

    static constexpr size_t FirstPiece { 512 };

private:
    // the first piece to the target: its check, then its bytes
    const char* start() {
        const char* error = _target.begin(_first, _first_len, _expected);
        if (error) {
            return fail(error);
        }
        _started = true;

        error = _target.write(_first, _first_len);
        if (error) {
            return fail(error);
        }

        return nullptr;
    }

    const char* fail(const char* error) {
        abort();
        _error = error;
        return error;
    }

    Target& _target;
    size_t _expected { 0 };
    size_t _received { 0 };
    uint8_t _first[FirstPiece] {};
    size_t _first_len { 0 };
    bool _started { false };
    bool _done { false };
    const char* _error { nullptr };
    char _text[64] {};
};

// The POSTs that change the firmware, every server: a browser sends Origin with them, a page of another site must not
// make the board flash something (the browser keeps the credentials of the board). No Origin at all is a client that
// is not a browser (curl, the release scripts), it passes; "null" does not. Behind a reverse proxy the Host the board
// sees is its own address: the proxy has to pass the one of the browser, as Host or as X-Forwarded-Host
inline bool sameOrigin(const char* origin, const char* host, const char* forwarded_host) {
    if (!origin || !*origin) {
        return true;
    }

    const char* scheme = strstr(origin, "://");
    if (!scheme) {
        return false;
    }

    const char* from = scheme + 3;
    return (host && *host && !strcasecmp(from, host))
        || (forwarded_host && *forwarded_host && !strcasecmp(from, forwarded_host));
}

} // namespace image
} // namespace espurna
