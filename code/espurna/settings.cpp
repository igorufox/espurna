/*

SETTINGS MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#include "espurna.h"

#include "crash.h"
#include "terminal.h"
#include "storage_eeprom.h"

#include <algorithm>
#include <vector>
#include <cstdlib>


// -----------------------------------------------------------------------------

namespace espurna {
namespace settings {

// Allow std::move() into the actual storage instance
EepromStorage::EepromStorage(EepromStorage&&) = default;
EepromStorage& EepromStorage::operator=(EepromStorage&&) = default;

// Depending on features enabled, we may end up with different left boundary
// Settings are written right-to-left, so we only have issues when there are a lot of key-values
kvs_type& kvs_instance() {
    static kvs_type storage(
        EepromStorage{eepromInstance()},
#if DEBUG_SUPPORT
        EepromReservedSize + crashReservedSize(),
#else
        EepromReservedSize,
#endif
        EepromSize
    );

    return storage;
}

namespace query {

espurna::string Result::value() const {
    if (_value) {
        return *_value;
    }

    if (_ptr == nullptr) {
        return espurna::string();
    }

    return (_index == IndexMax)
        ? (*reinterpret_cast<const Setting*>(_ptr)).value()
        : (*reinterpret_cast<const IndexedSetting*>(_ptr)).value(_index);
}

Result findFrom(const Setting* begin, const Setting* end, StringView key) {
    for (auto it = begin; it != end; ++it) {
        if ((*it) == key) {
            return Result(it);
        }
    }

    return Result(nullptr);
}

espurna::string findValueFrom(const Setting* begin, const Setting* end, StringView key) {
    espurna::string out;

    const auto result = findFrom(begin, end, key);
    if (result.ok()) {
        out = result.value();
    }

    return out;
}

const IndexedSetting* findSamePrefix(const IndexedSetting* begin, const IndexedSetting* end, StringView key) {
    const IndexedSetting* out { nullptr };

    for (auto it = begin; it != end; ++it) {
        if (key.startsWith((*it).prefix())) {
            out = it;
            break;
        }
    }

    return out;
}

Result findFrom(Iota iota, const IndexedSetting* begin, const IndexedSetting* end, StringView key) {
    while (iota) {
        for (auto it = begin; it != end; ++it) {
            const auto expected = Key(
                (*it).prefix().toString(), *iota);
            if (key == expected.value()) {
                return Result(it, *iota);
            }
        }
        ++iota;
    }

    return Result(nullptr);
}

espurna::string findValueFrom(Iota iota, const IndexedSetting* begin, const IndexedSetting* end, StringView key) {
    espurna::string out;

    const auto result = findFrom(iota, begin, end, key);
    if (result.pointer() != end) {
        out = result.value();
    }

    return out;
}

namespace internal {
namespace {

std::forward_list<Handler> handlers;

} // namespace
} // namespace internal

Result find(StringView key) {
    for (const auto& handler : internal::handlers) {
        if (handler.check != nullptr && !handler.check(key)) {
            continue;
        }

        auto result = handler.get(key);
        if (result.ok()) {
            return result;
        }
    }

    return Result(nullptr);
}

} // namespace query

namespace options {

bool EnumerationNumericHelper::check(const espurna::string& value) {
    if (value.length()) {
        if ((value.length() > 1) && (*value.begin() == '0')) {
            return false;
        }

        for (auto it = value.begin(); it != value.end(); ++it) {
            switch (*it) {
            case '0'...'9':
                break;
            default:
                return false;
            }
        }

        return true;
    }

    return false;
}

} // namespace options

ValueResult get(const espurna::string& key) {
    auto& kv_store = kvs_instance();
    return kv_store.get(key);
}

bool set(const espurna::string& key, const espurna::string& value) {
    auto& kv_store = kvs_instance();
    return kv_store.set(key, value);
}

bool del(const espurna::string& key) {
    auto& kv_store = kvs_instance();
    return kv_store.del(key);
}

bool has(const espurna::string& key) {
    auto& kv_store = kvs_instance();
    return kv_store.has(key);
}

Keys keys() {
    Keys out;
    foreach([&](kvs_type::KeyValueResult&& kv) {
        out.push_back(kv.key.toString());
    });

    return out;
}

size_t available() {
    auto& kv_store = kvs_instance();
    return kv_store.available();
}

size_t size() {
    auto& kv_store = kvs_instance();
    return kv_store.size();
}

void foreach(KeyValueResultCallback&& callback) {
    auto& kv_store = kvs_instance();
    kv_store.foreach(callback);
}

void foreach(KeyValueResultWithTokenCallback&& callback) {
    auto& kv_store = kvs_instance();
    kv_store.foreach(callback);
}

void foreach_prefix(PrefixResultCallback&& callback, query::StringViewIterator prefixes) {
    foreach([&](kvs_type::KeyValueResult&& kv) {
        auto key = kv.key.toString();
        for (auto it = prefixes.begin(); it != prefixes.end(); ++it) {
            if (StringView{key}.startsWith(*it)) {
                callback((*it), std::move(key), kv.value);
            }
        }
    });
}

// key movement - basic, indexed or range-indexed
namespace {

struct KeyPair {
    Key from;
    Key to;
};

bool move(kvs_type& instance, KeyPair pair) {
    const auto result = instance.get(pair.from.value());
    if (result) {
        instance.set(pair.to.value(), result.ref());
        instance.del(pair.from.value());
        return true;
    }

    return false;
}

bool move(const KeyPair& pair) {
    return move(kvs_instance(), pair);
}

bool move_index(kvs_type& instance, const espurna::string& from, const espurna::string& to, size_t index) {
    return move(instance,
        KeyPair{
            .from = {from, index},
            .to = {to, index}
        });
}

bool move_index(const espurna::string& from, const espurna::string& to, size_t index) {
    return move_index(kvs_instance(), from, to, index);
}

constexpr auto MovePrefixLimit = size_t{ 100 };

bool move_prefix(kvs_type& instance, const espurna::string& from, const espurna::string& to, size_t limit) {
    for (size_t index = 0; index < limit; ++index) {
        if (!move_index(instance, from, to, index)) {
            return false;
        }
    }

    return true;
}

bool move_prefix(const espurna::string& from, const espurna::string& to) {
    return move_prefix(kvs_instance(), from, to, MovePrefixLimit);
}

} // namespace

// --------------------------------------------------------------------------

// TODO: UI needs this to avoid showing keys in storage order
std::vector<espurna::string> sorted_keys() {
    auto values = keys();
    std::sort(values.begin(), values.end(),
        [](const espurna::string& lhs, const espurna::string& rhs) -> bool {
            return rhs.compareTo(lhs) > 0;
        });

    return values;
}

#if TERMINAL_SUPPORT
namespace terminal {
namespace {

void dump_impl(const ::terminal::CommandContext& ctx, const Key& key, const espurna::string& value) {
    ctx.output.printf_P(
        PSTR("> %s => %s%s\n"),
        key.c_str(), value.c_str(),
        hasSetting(key) ? " (*)" : "");
}

void dump(const ::terminal::CommandContext& ctx, const query::Setting* begin, const query::Setting* end) {
    for (auto it = begin; it != end; ++it) {
        dump_impl(ctx, (*it).key(), (*it).value());
    }
}

void dump(const ::terminal::CommandContext& ctx, const query::IndexedSetting* begin, const query::IndexedSetting* end, size_t index) {
    for (auto it = begin; it != end; ++it) {
        dump_impl(ctx, {(*it).prefix(), index}, (*it).value(index));
    }
}

namespace commands {

PROGMEM_STRING(Config, "CONFIG");

void config(::terminal::CommandContext&& ctx) {
    settingsPrintJson(ctx.output, true);
    ctx.output.print(static_cast<char>(10));
    terminalOK(ctx);
}

PROGMEM_STRING(Keys, "KEYS");

void keys(::terminal::CommandContext&& ctx) {
    const auto keys = settings::sorted_keys();

    espurna::string value;
    for (const auto& key : keys) {
        value = getSetting(key);
        ctx.output.printf_P(PSTR("> %s => \"%s\"\n"),
            key.c_str(), value.c_str());
    }

    const auto size = settings::size();
    if (size > 0) {
        const auto available = settings::available();
        ctx.output.printf_P(PSTR("Number of keys: %u\n"), keys.size());
        ctx.output.printf_P(PSTR("Available: %u bytes (%u%%)\n"),
                available, (100 * available) / size);
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Gc, "GC");

void gc(::terminal::CommandContext&& ctx) {
    struct KeyRef {
        espurna::string key;
        size_t length;
    };

    using KeyRefs = std::vector<KeyRef>;
    KeyRefs refs;

    auto& kv_store = kvs_instance();
    kv_store.foreach([&](kvs_type::KeyValueResult&& result) {
        refs.push_back(
            KeyRef{
                .key = result.key.toString(),
                .length = result.key.valueLength(),
            });
    });

    auto is_ascii = [](const espurna::string& value) -> bool {
        for (const auto& c : value) {
            if (!isascii(c)) {
                return false;
            }
        }

        return true;
    };

    std::vector<const espurna::string*> broken;
    for (const auto& ref : refs) {
        if ((ref.length != ref.key.length()) || !is_ascii(ref.key)) {
            broken.push_back(&ref.key);
        }
    }

    size_t count = 0;
    for (const auto& key : broken) {
        settings::del(*key);
        ++count;
    }

    ctx.output.printf_P("deleted %zu keys\n", count);
    terminalOK(ctx);
}

PROGMEM_STRING(Del, "DEL");

void del(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() < 2) {
        terminalError(ctx, "del <key> [<key>...]");
        return;
    }

    int result = 0;
    for (auto it = (ctx.argv.begin() + 1); it != ctx.argv.end(); ++it) {
        result += settings::del(*it);
    }

    if (result) {
        terminalOK(ctx);
    } else {
        terminalError(ctx, "no keys were removed");
    }
}

PROGMEM_STRING(Set, "SET");

void set(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, "set <key> <value>");
        return;
    }

    if (settings::set(ctx.argv[1], ctx.argv[2])) {
        terminalOK(ctx);
        return;
    }

    terminalError(ctx, "could not set the key");
}

PROGMEM_STRING(Get, "GET");

void get(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() < 2) {
        terminalError(ctx, "get <key> [<key>...]");
        return;
    }

    auto& instance = settings::kvs_instance();

    for (auto it = (ctx.argv.cbegin() + 1); it != ctx.argv.cend(); ++it) {
        auto result = instance.get(*it);
        if (!result) {
            const auto result = query::find(*it);
            if (result.ok()) {
                ctx.output.printf_P(PSTR("> %s => %s (default)\n"),
                    (*it).c_str(), result.value().c_str());
            } else {
                ctx.output.printf_P(PSTR("> %s =>\n"), (*it).c_str());
            }
            continue;
        }

        ctx.output.printf_P(PSTR("> %s => \"%s\"\n"), (*it).c_str(), result.c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Reload, "RELOAD");

void reload(::terminal::CommandContext&& ctx) {
    espurnaReload();
    terminalOK(ctx);
}

PROGMEM_STRING(FactoryReset, "FACTORY.RESET");

void factory_reset(::terminal::CommandContext&& ctx) {
    factoryReset();
    terminalOK(ctx);
}

[[gnu::unused]]
PROGMEM_STRING(Save, "SAVE");

[[gnu::unused]]
void save(::terminal::CommandContext&& ctx) {
    eepromCommit();
    terminalOK(ctx);
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Config, commands::config},
    {Keys, commands::keys},
    {Gc, commands::gc},

    {Del, commands::del},
    {Set, commands::set},
    {Get, commands::get},

    {Reload, commands::reload},
    {FactoryReset, commands::factory_reset},

#if not SETTINGS_AUTOSAVE
    {Save, commands::save},
#endif
};

} // namespace commands

void setup() {
    espurna::terminal::add(commands::List);
}

} // namespace
} // namespace terminal
#endif

} // namespace settings
} // namespace espurna

// -----------------------------------------------------------------------------
// Key-value API
// -----------------------------------------------------------------------------

size_t settingsSize() {
    auto& instance = espurna::settings::kvs_instance();
    return instance.size() - instance.available();
}

espurna::settings::Keys settingsKeys() {
    return espurna::settings::sorted_keys();
}

void settingsRegisterQueryHandler(espurna::settings::query::Handler handler) {
    espurna::settings::query::internal::handlers.push_front(handler);
}

espurna::settings::query::Result settingsQuery(espurna::StringView key) {
    return espurna::settings::query::find(key);
}

bool moveSetting(const espurna::string& from, const espurna::string& to) {
    return espurna::settings::move({from, to});
}

bool moveSetting(const espurna::string& from, const espurna::string& to, size_t index) {
    return espurna::settings::move_index(from, to, index);
}

bool moveSettings(const espurna::string& from, const espurna::string& to) {
    return espurna::settings::move_prefix(from, to);
}

template
bool getSetting(const espurna::settings::Key& key, bool defaultValue);

template
int getSetting(const espurna::settings::Key& key, int defaultValue);

template
long getSetting(const espurna::settings::Key& key, long defaultValue);

template
unsigned char getSetting(const espurna::settings::Key& key, unsigned char defaultValue);

template
unsigned short getSetting(const espurna::settings::Key& key, unsigned short defaultValue);

template
unsigned int getSetting(const espurna::settings::Key& key, unsigned int defaultValue);

template
unsigned long getSetting(const espurna::settings::Key& key, unsigned long defaultValue);

template
float getSetting(const espurna::settings::Key& key, float defaultValue);

template
double getSetting(const espurna::settings::Key& key, double defaultValue);

espurna::string getSettingString(const espurna::string& key) {
    return std::move(espurna::settings::get(key)).get();
}

espurna::string getSetting(const char* key) {
    return getSetting(espurna::settings::Key(key));
}

espurna::string getSetting(const espurna::settings::Key& key) {
    return getSetting(key, espurna::StringView(""));
}

espurna::string getSetting(const espurna::settings::Key& key, const char* defaultValue) {
    return getSetting(key, espurna::StringView(defaultValue));
}

espurna::string getSetting(const espurna::settings::Key& key, const espurna::string& defaultValue) {
    auto result = espurna::settings::get(key.value());
    if (result) {
        return std::move(result).get();
    }

    return defaultValue;
}

espurna::string getSetting(const espurna::settings::Key& key, espurna::string&& defaultValue) {
    espurna::string out;

    auto result = espurna::settings::get(key.value());
    if (result) {
        out = std::move(result).get();
    } else {
        out = std::move(defaultValue);
    }

    return out;
}

espurna::string getSetting(const espurna::settings::Key& key, espurna::StringView defaultValue) {
    espurna::string out;

    auto result = espurna::settings::get(key.value());
    if (result) {
        out = std::move(result).get();
    } else {
        out = defaultValue.toString();
    }

    return out;
}

bool delSettingString(const espurna::string& key) {
    return espurna::settings::del(key);
}

bool delSetting(const espurna::settings::Key& key) {
    return delSetting(key.value());
}

bool delSetting(const char* key) {
    return delSettingString(espurna::string(key));
}

bool hasSettingString(const espurna::string& key) {
    return espurna::settings::has(key);
}

bool hasSetting(const espurna::settings::Key& key) {
    return hasSetting(key.value());
}

bool hasSetting(const char* key) {
    return hasSettingString(espurna::string(key));
}

void saveSettings() {
#if not SETTINGS_AUTOSAVE
    eepromCommit();
#endif
}

void autosaveSettings() {
#if SETTINGS_AUTOSAVE
    eepromCommit();
#endif
}

void resetSettings() {
    eepromClear();
}

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

// {"key": "value", ...} of every setting: the backup file (GET /config) and terminal CONFIG. Printed here, the head has
// no JSON code (the restore reads it in the ws module, settings_json.cpp)
void settingsPrintJson(espurna::Output& out, bool pretty) {
    const auto quote = [&](espurna::StringView text) {
        out.write('"');
        const char* start = text.begin();
        for (const char* ptr = text.begin(); ptr != text.end(); ++ptr) {
            const auto c = static_cast<unsigned char>(*ptr);
            if ((c >= 0x20) && (c != '"') && (c != 0x5c)) {
                continue;
            }

            out.write(start, ptr - start);
            start = ptr + 1;
            if ((c == '"') || (c == 0x5c)) {
                out.write(static_cast<char>(0x5c));
                out.write(static_cast<char>(c));
            } else {
                out.printf("%cu%04x", 0x5c, static_cast<unsigned>(c));
            }
        }
        out.write(start, text.end() - start);
        out.write('"');
    };

    const auto app = buildApp();
    out.write('{');
    if (pretty) {
        out.write(static_cast<char>(10));
    }
    quote(STRING_VIEW("app"));
    out.write(':');
    quote(app.name);
    out.write(',');
    quote(STRING_VIEW("version"));
    out.write(':');
    quote(app.version);
    out.write(',');
    quote(STRING_VIEW("backup"));
    out.write(':');
    quote(STRING_VIEW("1"));

    espurna::settings::foreach([&](espurna::settings::kvs_type::KeyValueResult&& kv) {
        const auto key = kv.key.toString();
        const auto value = kv.value.toString();
        out.write(',');
        if (pretty) {
            out.write(static_cast<char>(10));
        }
        quote(key);
        out.write(':');
        quote(value);
    });

    if (pretty) {
        out.write(static_cast<char>(10));
    }
    out.write('}');
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT

void settingsDump(const ::terminal::CommandContext& ctx,
        const espurna::settings::query::Setting* begin,
        const espurna::settings::query::Setting* end)
{
    espurna::settings::terminal::dump(ctx, begin, end);
}

void settingsDump(const ::terminal::CommandContext& ctx,
        const espurna::settings::query::IndexedSetting* begin,
        const espurna::settings::query::IndexedSetting* end, size_t index)
{
    espurna::settings::terminal::dump(ctx, begin, end, index);
}

#endif

void settingsSetup() {
#if TERMINAL_SUPPORT
    espurna::settings::terminal::setup();
#endif
}

