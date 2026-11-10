/*

PROTOTYPE: the ESP32 test module (docs/modules.md, "ESP32"), env:esp32-modules of
platformio_esp32.ini. Exercises every kind of relocation and a call into the head of each sort:
a global object with a constructor (init array), .data and .bss, strings and a vtable in the data view, a timer of the
head calling back into the module, a terminal command.

*/

#include "espurna.h"

#if MODULE_BUILD && defined(ESP32)

#include "modules.h"
#include "terminal.h"

namespace espurna {
namespace modules {
namespace test {
namespace {

struct Counter {
    virtual ~Counter() = default;
    virtual void tick() = 0;
    virtual uint32_t value() const = 0;
};

struct Ticks : public Counter {
    void tick() override {
        ++_ticks;
    }

    uint32_t value() const override {
        return _ticks;
    }

private:
    uint32_t _ticks { 0 };
};

Ticks ticks;                                // .bss + vtable in .rodata
uint32_t started { 0x5a5a0001 };            // .data
timer::SystemTimer timer;                   // constructor of the head, from the module's init array
const char* const Names[] { "one", "two", "three" };    // pointers in .rodata

void command(::terminal::CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("ticks %u, started 0x%08x, %s, code %p, data %p, rodata %p\n"),
        ticks.value(), started, Names[ticks.value() % 3],
        reinterpret_cast<void*>(&command), &started, &Names[0]);
    terminalOK(ctx);
}

} // namespace
} // namespace test
} // namespace modules
} // namespace espurna

ESPURNA_MODULE(test, "test", 90) {
    using namespace espurna::modules::test;
    started = millis();
    timer.repeat(espurna::duration::Seconds(1), []() {
        ticks.tick();
    });
    espurna::terminal::add(STRING_VIEW("MODTEST"), command);
    DEBUG_MSG_P(PSTR("[MODTEST] started at %u ms\n"), started);
}

#endif
