/*

A byte stream holding both input and output: what is written is read back. Only used in tests (code/test/unit).

*/

#pragma once

#include "../byte_stream.h"

#include <algorithm>
#include <cstring>
#include <vector>

class StreamEcho final : public espurna::ByteStream {
public:
    using espurna::ByteStream::read;
    using espurna::ByteStream::write;

    void write(const char* data, size_t size) override {
        _data.insert(_data.end(), data, data + size);
    }

    // the same as write(), the name of a string
    void concat(const char* data, size_t size) {
        write(data, size);
    }

    void flush() override {
        _data.clear();
    }

    size_t available() override {
        return _data.size();
    }

    size_t length() const {
        return _data.size();
    }

    int read() override {
        if (_data.empty()) {
            return -1;
        }

        const auto out = static_cast<unsigned char>(_data.front());
        _data.erase(_data.begin());
        return out;
    }

    size_t read(uint8_t* data, size_t size) override {
        const auto need = std::min(size, _data.size());
        std::memcpy(data, _data.data(), need);
        _data.erase(_data.begin(), _data.begin() + need);
        return need;
    }

    int peek() override {
        if (_data.empty()) {
            return -1;
        }

        return static_cast<unsigned char>(_data.front());
    }

private:
    std::vector<char> _data;
};
