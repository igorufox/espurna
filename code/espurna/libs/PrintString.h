/*

Arduino Print buffer. Size is fixed, unlike StreamString.

Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once

#include "../text_types.h"

struct PrintString final : public espurna::Output, public espurna::string {
    PrintString() = delete;
    PrintString(size_t reserved) :
        _reserved(reserved)
    {
        reserve(reserved);
    }

    using espurna::Output::write;

    void write(const char* data, size_t size) override {
        if (!size || !data) {
            return;
        }

        // we *will* receive C-strings as input
        size_t want = length() + size;
        if (data[size - 1] == '\0') {
            size -= 1;
            want -= 1;
        }

        if (want > _reserved) {
            return;
        }

        concat(data, size);
    }

private:
    size_t _reserved;
};

// grows as it is written to, nothing reserved ahead
struct StringPrint final : public espurna::Output, public espurna::string {
    using espurna::Output::write;

    void write(const char* data, size_t size) override {
        concat(data, size);
    }
};
