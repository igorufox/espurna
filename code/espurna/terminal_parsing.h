/*

Part of the TERMINAL MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once


#include <cstring>
#include <iterator>
#include <vector>

#include "types.h"
#include "text_types.h"

namespace espurna {
namespace terminal {
namespace parser {

enum class Error {
    Uninitialized,     // parser never started / no text
    Busy,              // parser was already parsing something
    InvalidEscape,     // escaped text was invalid
    NoSpaceAfterQuote, // parsing stopped since there was no space after quote
    UnexpectedLineEnd, // unexpected \r encountered in the input
    UnterminatedQuote, // parsing stopped without terminating a quoted entry
    Ok,                // success
};

espurna::string error(Error);

} // namespace parser

// Type wrapper that flushes output on finding '\n'.
// Inherit from `espurna::string` to reuse its buffer for every line.
// (and also seamlessly behave like a usual `espurna::string`, we import most of its methods)
template <typename T>
class PrintLine final : public ::espurna::Output, public espurna::string {
private:
    static_assert(!std::is_reference<T>::value, "");

public:
    using espurna::string::begin;
    using espurna::string::end;
    using espurna::string::reserve;
    using espurna::string::c_str;
    using espurna::string::length;

    PrintLine() = default;
    ~PrintLine() {
        send();
    }

    template <typename... Args>
    PrintLine(Args&&... args) :
        _output(std::forward<Args>(args)...)
    {}

    T& output() {
        return _output;
    }

    void flush() override {
        if (end() != std::find(begin(), end(), '\n')) {
            send();
            // keeps the buffer, only the length and the terminator change
            remove(0);
        }
    }

    using ::espurna::Output::write;

    void write(const char* data, size_t size) override {
        if (!_lock && size && data && *data != '\0') {
            ReentryLock lock(_lock);

            concat(data, size);
            flush();
        }
    }

private:
    void send() {
        _output.write(c_str(), length());
    }

    T _output;
    bool _lock { false };
};

using Tokens = std::vector<StringView>;
using Buffer = std::vector<espurna::string>;

// Values of every token from the input on success (Error::Ok)
// Remaining part of the string (when terminated early)
// Empty vector and parsing error otherwise
struct ParsedLine {
    Tokens tokens;
    Buffer buffer;
    StringView remaining;
    parser::Error error;
};

ParsedLine parse_line(StringView line);
ParsedLine parse_terminated(StringView line);

} // namespace terminal
} // namespace espurna

