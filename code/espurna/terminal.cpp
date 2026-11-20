/*

TERMINAL MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if TERMINAL_SUPPORT

#if API_SUPPORT
#include "api.h"
#endif

#if WEB_SUPPORT
#include "ws.h"
#endif

#include "crash.h"
#include "mqtt.h"
#include "settings.h"
#include "system.h"
#include "sensor.h"
#include "telnet.h"
#include "terminal.h"
#include "utils.h"
#include "espurna_wifi.h"

#if MODULES_SUPPORT
#include "modules.h"
#endif

#include "libs/PrintString.h"
#include "libs/Delimiter.h"

#include <algorithm>
#include <utility>

#include "espurna_chip.h"


// FS 'range', declared at compile time via .ld script PROVIDE declarations
// (althought, in recent Core versions, these may be set at runtime)
extern "C" uint32_t _FS_start;
extern "C" uint32_t _FS_end;


#if WEB_SUPPORT
#include "web.h"
#endif

namespace espurna {
namespace terminal {

void add(StringView name, CommandFunc func) {
    const auto cmd = new Command{
        .name = name,
        .func = func,
    };

    add(Commands{
        .begin = cmd,
        .end = cmd + 1,
    });
}

namespace {

namespace build {

constexpr size_t serialBufferSize() {
    return TERMINAL_SERIAL_BUFFER_SIZE;
}

constexpr size_t serialPort() {
    return TERMINAL_SERIAL_PORT - 1;
}

} // namespace build

// -----------------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------------

namespace commands {

PROGMEM_STRING(Commands, "COMMANDS");
PROGMEM_STRING(Help, "HELP");

void help(CommandContext&& ctx) {
    auto names = terminal::names();

    std::sort(names.begin(), names.end(),
        [](StringView lhs, StringView rhs) {
            // XXX: Core's ..._P funcs only allow 2nd pointer to be in PROGMEM,
            //      explicitly load the 1st one
            // TODO: can we just assume linker already sorted all strings?
            return strncasecmp_P(
                lhs.toString().begin(), rhs.begin(), lhs.length()) < 0;
        });

    ctx.output.print("Available commands:\n");
    for (auto name : names) {
        ctx.output.printf("> %s\n", name.c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(LightSleep, "SLEEP.LIGHT");

void light_sleep(CommandContext&& ctx) {
    if (ctx.argv.size() == 2) {
        using namespace espurna::duration;

        const auto result = parse(ctx.argv[1], std::micro{});
        if (!result.ok) {
            terminalError(ctx, "Invalid time");
            return;
        }

        const auto duration = to_chrono<sleep::Microseconds>(result.value);
        if (!instantLightSleep(duration)) {
            terminalError(ctx, "Could not sleep");
            return;
        }

        return;
    }

    instantLightSleep();
}

PROGMEM_STRING(DeepSleep, "SLEEP.DEEP");

void deep_sleep(CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, "SLEEP.DEEP <TIME (MICROSECONDS)>");
        return;
    }

    using namespace espurna::duration;
    const auto result = parse(ctx.argv[1], std::micro{});

    if (!result.ok) {
        terminalError(ctx, "Invalid time");
        return;
    }

    const auto duration = to_chrono<sleep::Microseconds>(result.value);
    if (!instantDeepSleep(duration)) {
        terminalError(ctx, "Could not sleep");
        return;
    }
}

PROGMEM_STRING(Reset, "RESET");
// the same restart under the name people type first
PROGMEM_STRING(Reboot, "REBOOT");

void reset(CommandContext&& ctx) {
    prepareReset(CustomResetReason::Terminal);
    terminalOK(ctx);
}

#if defined(ESP8266)
PROGMEM_STRING(ResetRom, "RESET.ROM");

// the ROM's UART download mode, no GPIO0 jumper: esptool --before no-reset --no-stub writes the flash right after it
void reset_rom(CommandContext&& ctx) {
    terminalOK(ctx);
    espurnaRegisterOnce([]() {
        espurna::time::sleep(200);
        espurna::core::chip::rom_loader();
    });
}

#endif


PROGMEM_STRING(EraseConfig, "ERASE.CONFIG");

void erase_config(CommandContext&& ctx) {
    terminalOK(ctx);
    customResetReason(CustomResetReason::Terminal);
    forceEraseSDKConfig();
}

PROGMEM_STRING(Heap, "HEAP");

void heap(CommandContext&& ctx) {
    const auto stats = systemHeapStats();
    ctx.output.printf_P(PSTR("initial: %lu available: %lu contiguous: %lu\n"),
            systemInitialFreeHeap(), stats.available, stats.usable);
    espurna::chip::memory(ctx.output);

    terminalOK(ctx);
}

PROGMEM_STRING(Uptime, "UPTIME");

void uptime(CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("uptime %s\n"),
        prettyDuration(systemUptime()).c_str());
    terminalOK(ctx);
}

PROGMEM_STRING(Info, "INFO");

void info(CommandContext&& ctx) {
    const auto app = buildApp();
    ctx.output.printf_P(PSTR("%s %s built %s\n"),
            app.name.c_str(), app.version.c_str(), app.build_time.c_str());

    ctx.output.printf_P(PSTR("device: %s\n"),
            systemDevice().c_str());
    espurna::chip::info(ctx.output);

    const auto sdk = buildSdk();
    ctx.output.printf_P(PSTR("sdk: %s core: %s\n"),
            sdk.base.c_str(), sdk.version.c_str());
#if MODULES_SUPPORT
    // the head's identity (its build id), under the name the tools read (scripts/bench_regress.py)
    ctx.output.printf_P(PSTR("md5: %s\n"), modulesHeadId().c_str());
#endif

    const auto modules = buildModules();
    ctx.output.printf_P(PSTR("support: %.*s\n"),
        modules.length(), modules.c_str());

#if SENSOR_SUPPORT
    const auto sensors = sensorList();
    ctx.output.printf_P(PSTR("sensors: %.*s\n"),
        sensors.length(), sensors.c_str());
#endif

#if SYSTEM_CHECK_ENABLED
    ctx.output.printf_P(PSTR("system: %s boot counter: %u\n"),
        systemCheck()
            ? PSTR("OK")
            : PSTR("UNSTABLE"),
        systemStabilityCounter());
#endif

#if DEBUG_SUPPORT
    crashResetReason(ctx.output);
#endif

    terminalOK(ctx);
}

struct Layout {
    Layout() = delete;

    constexpr Layout(const Layout&) = default;
    constexpr Layout(Layout&&) = default;
    constexpr Layout(const char* const name, uint32_t start, uint32_t end) :
        _name(name),
        _start(start),
        _end(end)
    {}

    constexpr uint32_t size() const {
        return _end - _start;
    }

    constexpr uint32_t start() const {
        return _start;
    }

    constexpr uint32_t end() const {
        return _end;
    }

    constexpr const char* name() const {
        return _name;
    }

private:
    const char* const _name;
    uint32_t _start;
    uint32_t _end;
};

struct Layouts {
    using List = std::forward_list<Layout>;

    Layouts() = delete;
    explicit Layouts(uint32_t size) :
        _size(size),
        _current(size),
        _sectors(size / SPI_FLASH_SEC_SIZE)
    {}

    const Layout* head() const {
        if (_list.empty()) {
            return nullptr;
        }

        return &_list.front();
    }

    bool lock() {
        if (_lock) {
            return true;
        }

        _lock = true;
        return false;
    }

    uint32_t sectors() const {
        return _sectors;
    }

    uint32_t size() const {
        return _size - _current;
    }

    uint32_t current() const {
        return _current;
    }

    Layouts& add(const char* const name, uint32_t size) {
        if (!_lock && _current >= size) {
            Layout layout(name, _current - size, _current);
            _current -= layout.size();
            _list.push_front(layout);
        }

        return *this;
    }

    template <typename T>
    void foreach(T&& callback) {
        for (auto& layout : _list) {
            callback(layout);
        }
    }

private:
    bool _lock { false };
    List _list;
    uint32_t _size;
    uint32_t _current;
    uint32_t _sectors;
};

StringView flash_chip_mode() {
    static const espurna::string out = ([]() -> espurna::string {
        // the SPI mode of the flash chip as the image header has it: 0 QIO, 1 QOUT, 2 DIO, 3 DOUT
        switch (espurna::core::chip::flash_mode()) {
        case 0:
            return PSTR("QIO");
        case 1:
            return PSTR("QOUT");
        case 2:
            return PSTR("DIO");
        case 3:
            return PSTR("DOUT");
        }

        return PSTR("UNKNOWN");
    })();

    return out;
}

PROGMEM_STRING(Storage, "STORAGE");

void storage(CommandContext&& ctx) {
    // partitions (ESP32), the chip describes them itself
    if (espurna::chip::storage(ctx.output, flash_chip_mode().c_str())) {
        terminalOK(ctx);
        return;
    }

    ctx.output.printf_P(PSTR("flash chip ID: 0x%06X\n"), ESP_COMPAT.getFlashChipId());
    ctx.output.printf_P(PSTR("speed: %u\n"), espurna::core::chip::flash_speed());
    ctx.output.printf_P(PSTR("mode: %s\n"), flash_chip_mode().c_str());

    ctx.output.printf_P(PSTR("size: %u (SPI), %u (SDK)\n"),
        espurna::core::chip::flash_size(), espurna::core::chip::flash_size());

    Layouts layouts(espurna::core::chip::flash_size());

    // SDK specifies a hard-coded layout, there's no data beyond
    // (...addressable by the Core, since it adheres the setting)
    if (espurna::core::chip::flash_size() > espurna::core::chip::flash_size()) {
        layouts.add("unused", espurna::core::chip::flash_size() - espurna::core::chip::flash_size());
    }

    // app is at a normal location, [0...size), but... since it is offset by the free space, make sure it is aligned
    // to the sector size (...and it is expected from the getFreeSketchSpace, as the app will align to use the fixed
    // sector address for OTA writes).
    layouts.add("sdk", 4 * SPI_FLASH_SEC_SIZE);
    layouts.add("eeprom", eepromSpace());

    auto app_size = (espurna::core::chip::sketch_size() + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
    auto ota_size = layouts.current() - app_size;

    // OTA is allowed to use all but one eeprom sectors that, leaving the last one
    // for the settings snapshot during the update
    layouts.add("ota", ota_size);
    layouts.add("app", app_size);

    layouts.foreach(
        [&](const Layout& layout) {
            ctx.output.printf_P("%-6s [%08X...%08X) (%u bytes)\n",
                    layout.name(), layout.start(), layout.end(), layout.size());
        });

    terminalOK(ctx);
}

PROGMEM_STRING(Adc, "ADC");

void adc(CommandContext&& ctx) {
    const int pin = (ctx.argv.size() == 2)
        ? ctx.argv[1].toInt()
        : espurna::pins::Adc0;

    ctx.output.printf_P(PSTR("value %d\n"), espurna::pins::analog(pin));
    terminalOK(ctx);
}

#if SYSTEM_CHECK_ENABLED
PROGMEM_STRING(Stable, "STABLE");

void stable(CommandContext&&) {
    systemForceStable();
    prepareReset(CustomResetReason::Stability);
}

PROGMEM_STRING(Unstable, "UNSTABLE");

void unstable(CommandContext&&) {
    systemForceUnstable();
    prepareReset(CustomResetReason::Stability);
}

PROGMEM_STRING(Trap, "TRAP");

void trap(CommandContext&&) {
    __builtin_trap();
}

#endif

static constexpr ::terminal::Command List[] PROGMEM {
    {Commands, commands::help},
    {Help, commands::help},

    {Info, commands::info},
    {Storage, commands::storage},
    {Uptime, commands::uptime},
    {Heap, commands::heap},

    {Adc, commands::adc},

    {LightSleep, commands::light_sleep},
    {DeepSleep, commands::deep_sleep},

    {Reset, commands::reset},
    {Reboot, commands::reset},
    {EraseConfig, commands::erase_config},
#if defined(ESP8266)
    {ResetRom, commands::reset_rom},
#endif

#if SYSTEM_CHECK_ENABLED
    {Stable, commands::stable},
    {Unstable, commands::unstable},
    {Trap, commands::trap},
#endif
};

void setup() {
    espurna::terminal::add(List);
}

} // namespace commands

#if TERMINAL_SERIAL_SUPPORT
namespace serial {

using LoopFunc = void(*)();
void empty_loop() {
}

namespace internal {

::espurna::ByteStream* stream { nullptr };
LoopFunc loop { empty_loop };

} // namespace internal

void processing_loop() {
    using LineBuffer = LineBuffer<build::serialBufferSize()>;
    static LineBuffer buffer;

    auto& port = *internal::stream;
    const auto available = port.available();
    if (available <= 0) {
        return;
    }

    buffer.append(port, available);

    if (buffer.overflow()) {
        terminal::error(port, "Serial buffer overflow");
        buffer.reset();
    }

    for (;;) {
        const auto result = buffer.next();
        if (result.overflow) {
            terminal::error(port, "Command line buffer overflow");
            continue;
        }

        if (!result.value.length()) {
            break;
        }

        api_find_and_call(result.value, port);
    }
}

void loop() {
    internal::loop();
}

// a sensor (or another driver) took the port, ref. uartClaim()
const char* release(size_t index) {
    if ((index != build::serialPort()) || !internal::stream) {
        return nullptr;
    }

    internal::loop = empty_loop;
    internal::stream = nullptr;

    return PSTR("terminal");
}

bool attach() {
    auto port = uartPort(build::serialPort());
    if (!port || (!port->rx || !port->tx) || uartClaimed(build::serialPort())) {
        return false;
    }

    internal::stream = port->stream;
    internal::loop = processing_loop;
    uartOnClaim(release);
    return true;
}

// the port comes with the `uart` module, which starts after the head's setup: until it is there, look for it once a
// second (19.11: the terminal never attached on the bench, the log ran but no command was read)
void waiting_loop() {
    static uint32_t last { 0 };
    const uint32_t now = espurna::time::millis().time_since_epoch().count();
    if (now - last < 1000) {
        return;
    }
    last = now;
    attach();
}

void setup() {
    if (!attach()) {
        internal::loop = waiting_loop;
    }
}

} // namespace serial
#endif

#if MQTT_SUPPORT && TERMINAL_MQTT_SUPPORT
namespace mqtt {

void setup() {
    mqttRegister([](unsigned int type, StringView topic, StringView payload) {
        if (type == MQTT_CONNECT_EVENT) {
            mqttSubscribe(MQTT_TOPIC_CMD);
            return;
        }

        if (type == MQTT_MESSAGE_EVENT) {
            auto t = mqttMagnitude(topic);
            if (!t.startsWith(MQTT_TOPIC_CMD)) {
                return;
            }

            if (!payload.length()) {
                return;
            }

            // TODO: unlike http handler, we have only one output stream
            //       and **must** have a fixed-size output buffer
            //       (wishlist: MQTT client does some magic and we don't buffer twice)
            // TODO: or, at least, make it growable on-demand and cap at MSS?
            // TODO: PrintLine<...> instead of one giant blob?

            auto ptr = std::make_shared<espurna::string>(payload.toString());

            espurnaRegisterOnce([ptr]() {
                PrintString out(TCP_MSS);
                api_find_and_call(*ptr, out);

                if (out.length()) {
                    static const auto topic = mqttTopic(MQTT_TOPIC_CMD);
                    mqttSendRaw(topic.c_str(), out.c_str(), false);
                }
            });

            return;
        }
    });

}

} // namespace mqtt
#endif



// -----------------------------------------------------------------------------
// Pubic API
// -----------------------------------------------------------------------------

#if TERMINAL_WEB_API_SUPPORT
namespace api {

STRING_VIEW_INLINE(Value, "line");

STRING_VIEW_INLINE(Path, TERMINAL_WEB_API_PATH);
STRING_VIEW_INLINE(Key, "termWebApiPath");

// XXX: new `apiRegister()` depends that `webServer()` is available, meaning we can't call this setup func
// before the `webSetup()` is called. ATM, just make sure it is in order.

using Commands = std::shared_ptr<std::vector<espurna::string>>;

#define API_UPDATE_COMMANDS(OUT, NAME, VALUE) \
    if (!OUT) {\
        OUT = std::make_shared<Commands::element_type>();\
    }\
\
    if (NAME == Value) {\
        (OUT)->push_back(VALUE);\
    }\

#define API_SCHEDULE_COMMANDS(REQUEST, COMMANDS) \
    espurna::web::print::scheduleFromRequest(\
        REQUEST,\
        [COMMANDS](::espurna::Output& out) {\
            for (const auto& cmd : *COMMANDS) {\
                api_find_and_call(cmd, out);\
            }\
        });\

void setup() {
#if API_SUPPORT
    apiRegister(
        getSetting(Key, Path),
        [](ApiRequest& api) {
            api.handle([](WebRequest& request) {
                espurna::string out;
                for (const auto view : names()) {
                    out += view.toString();
                    out += "\r\n";
                }

                webSend(request, 200, "text/plain", out);
            });

            return true;
        },
        [](ApiRequest& api) {
            Commands cmds;

            api.param_foreach(
                [&](const espurna::string& name, const espurna::string& value) {
                    API_UPDATE_COMMANDS(cmds, name, value);
                });

            if (!cmds || !cmds->size()) {
                return false;
            }

            api.handle([cmds](WebRequest& request) {
                API_SCHEDULE_COMMANDS(request, cmds);
            });

            return true;
        }
    );
#else
    webRequestRegister([](WebRequest& request) {
        STRING_VIEW_INLINE(BasePath, API_BASE_PATH);

        espurna::string path;
        path += BasePath;
        path += getSetting(Key, Path);

        if (!path.equals(request.url())) {
            return false;
        }

        if (!apiAuthenticate(request)) {
            request.send(403, "text/plain", "");
            return true;
        }

        Commands cmds;

        for (size_t n = 0; n < request.params(); ++n) {
            const espurna::string name(request.paramName(n));
            const espurna::string value(request.paramValue(n));
            if (!apiReservedParam(name)) {
                API_UPDATE_COMMANDS(cmds, name, value);
            }
        }

        if (!cmds || !cmds->size()) {
            request.send(500, "text/plain", "");
            return true;
        }

        API_SCHEDULE_COMMANDS(request, cmds);

        return true;
    });
#endif // API_SUPPORT
}

#undef API_SCHEDULE_COMMANDS
#undef API_UPDATE_COMMANDS

} // namespace api
#endif // TERMINAL_WEB_API_SUPPORT

void loop() {
#if TERMINAL_SERIAL_SUPPORT
    serial::loop();
#endif
}

void setup() {
#if TERMINAL_SERIAL_SUPPORT
    serial::setup();
#endif

#if MQTT_SUPPORT && TERMINAL_MQTT_SUPPORT
    // Similar to the above, accept cmdline(s) in payload
    mqtt::setup();
#endif

    // Initialize default commands
    commands::setup();

    // Register loop
    espurnaRegisterLoop(loop);
}

} // namespace
} // namespace terminal
} // namespace espurna

void terminalOK(const espurna::terminal::CommandContext& ctx) {
    espurna::terminal::ok(ctx);
}

void terminalError(const espurna::terminal::CommandContext& ctx) {
    espurna::terminal::error(ctx, espurna::string());
}

void terminalError(const espurna::terminal::CommandContext& ctx, const espurna::string& message) {
    espurna::terminal::error(ctx, message);
}

void terminalError(const espurna::terminal::CommandContext& ctx, const char* message) {
    espurna::terminal::error(ctx, espurna::string(message));
}

void terminalError(const espurna::terminal::CommandContext& ctx, espurna::StringView message) {
    espurna::terminal::error(ctx, message.toString());
}

void terminalRegisterCommand(espurna::terminal::Commands commands) {
    espurna::terminal::add(commands);
}

#if TERMINAL_WEB_API_SUPPORT
void terminalWebApiSetup() {
    espurna::terminal::api::setup();
}
#endif

void terminalSetup() {
    espurna::terminal::setup();
}

#endif // TERMINAL_SUPPORT


