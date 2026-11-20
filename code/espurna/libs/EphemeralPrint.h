/*

An output that drops everything.

*/

#pragma once

#include "../text_types.h"

struct EphemeralPrint : public espurna::Output {
    void write(const char*, size_t) override {
    }
};
