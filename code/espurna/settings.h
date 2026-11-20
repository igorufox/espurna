/*

SETTINGS MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020-2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once


#include <functional>
#include <utility>
#include <vector>
#include <type_traits>

#include "text_types.h"

#include "storage_eeprom.h"

#include "settings_convert.h"
#include "settings_helpers.h"
#include "settings_embedis.h"
#include "terminal.h"

// --------------------------------------------------------------------------

void resetSettings();
void saveSettings();
void autosaveSettings();

namespace espurna {
namespace settings {

class EepromStorage {
public:
    using storage_type = StorageEEPROM_Rotate;

    EepromStorage() = delete;

    EepromStorage(EepromStorage&) = delete;
    EepromStorage& operator=(EepromStorage&) = delete;

    EepromStorage(EepromStorage&&);
    EepromStorage& operator=(EepromStorage&&);

    explicit EepromStorage(storage_type& instance) :
        _instance(std::addressof(instance))
    {}

    std::vector<uint8_t> read(uint16_t pos, size_t size) const {
        const auto* ptr = _instance->data() + pos;
        std::vector<uint8_t> out;
        out.insert(out.end(), ptr, ptr + size);
        return out;
    }

    Span<const uint8_t> data(uint16_t pos, size_t size) const {
        const auto* ptr = _instance->data() + pos;
        return Span<const uint8_t>(ptr, size);
    }

    uint8_t read(uint16_t pos) const {
        return _instance->data()[pos];
    }

    void write(uint16_t pos, Span<const uint8_t> data) const {
        auto* ptr = const_cast<uint8_t*>(_instance->data()) + pos;
        memmove_P(ptr, data.data(), data.size());
    }

    void write(uint16_t pos, uint8_t value) const {
        write(pos, Span<const uint8_t>(&value, 1));
    }

    void fill(uint16_t pos, size_t size, uint8_t value) const {
        auto* ptr = const_cast<uint8_t*>(_instance->data()) + pos;
        std::fill(ptr, ptr + size, value);
    }

    void commit() const {
        _instance->setDirty();
        autosaveSettings();
    }

private:
    storage_type* _instance;
};

using kvs_type = embedis::KeyValueStore<EepromStorage>;

kvs_type& kvs_instance();

namespace traits {

template<typename T>
using is_arduino_string = std::is_same<espurna::string, typename std::decay<T>::type>;

template<typename T>
using enable_if_arduino_string = std::enable_if<is_arduino_string<T>::value>;

template<typename T>
using enable_if_not_arduino_string = std::enable_if<!is_arduino_string<T>::value>;

} // namespace types

// TODO: allow StringView as key and value
// does not work right now because embedis api only works on things with char access
// (won't work on our flash strings, since those can only be accessed via aligned reads)

ValueResult get(const espurna::string& key);
bool set(const espurna::string& key, const espurna::string& value);
bool del(const espurna::string& key);
bool has(const espurna::string& key);

using Keys = std::vector<espurna::string>;
Keys keys();

size_t available();
size_t size();

using KeyValueResultCallback = std::function<void(settings::kvs_type::KeyValueResult&&)>;
void foreach(KeyValueResultCallback&&);

using KeyValueResultWithTokenCallback = std::function<void(settings::kvs_type::KeyValueResult&&, settings::kvs_type::StopToken)>;
void foreach(KeyValueResultWithTokenCallback&&);

using PrefixResultCallback = std::function<void(StringView prefix, espurna::string key, const kvs_type::ReadResult& value)>;
void foreach_prefix(PrefixResultCallback&&, settings::query::StringViewIterator);

// --------------------------------------------------------------------------

namespace query {

using Check = bool(*)(StringView key);
using Get = Result(*)(StringView key);

struct Handler {
    Check check;
    Get get;
};

} // namespace query
} // namespace settings
} // namespace espurna

void settingsRegisterQueryHandler(espurna::settings::query::Handler);
espurna::settings::query::Result settingsQuery(espurna::StringView key);

// --------------------------------------------------------------------------

bool moveSetting(const espurna::string& from, const espurna::string& to, size_t index);
bool moveSetting(const espurna::string& from, const espurna::string& to);

bool moveSettings(const espurna::string& from, const espurna::string& to);

espurna::string getSetting(const char* key);
espurna::string getSettingString(const espurna::string& key);

// exactly a espurna::string (a braced {key, index} goes to the Key overloads, it does not deduce this)
template <typename T, typename = typename std::enable_if<std::is_same<T, espurna::string>::value>::type>
espurna::string getSetting(const T& key) {
    return getSettingString(key);
}

espurna::string getSetting(const espurna::settings::Key& key);
espurna::string getSetting(const espurna::settings::Key& key, const char* defaultValue);
espurna::string getSetting(const espurna::settings::Key& key, const espurna::string& defaultValue);
espurna::string getSetting(const espurna::settings::Key& key, espurna::string&& defaultValue);
espurna::string getSetting(const espurna::settings::Key& key, espurna::StringView defaultValue);

template <typename T, typename = typename espurna::settings::traits::enable_if_not_arduino_string<T>::type>
T getSetting(const espurna::settings::Key& key, T defaultValue) {
    auto result = espurna::settings::get(key.value());
    if (result) {
        return espurna::settings::internal::convert<T>(result.ref());
    }
    return defaultValue;
}

template <typename T>
inline bool setSetting(const espurna::settings::Key& key, T&& value) {
    return espurna::settings::set(key.value(), ::espurna::settings::internal::serialize(value));
}

template <>
inline bool setSetting(const espurna::settings::Key& key, const espurna::string& value) {
    return espurna::settings::set(key.value(), value);
}

template <>
inline bool setSetting(const espurna::settings::Key& key, espurna::string& value) {
    return espurna::settings::set(key.value(), std::cref(value).get());
}

inline bool setSetting(const espurna::settings::Key& key, espurna::string value) {
    return espurna::settings::set(key.value(), std::cref(value).get());
}

template <size_t Size>
inline bool setSetting(const espurna::settings::Key& key, const char (&value)[Size]) {
    return setSetting(key, espurna::string(value));
}

bool delSetting(const char* key);
bool delSettingString(const espurna::string& key);

template <typename T, typename = typename std::enable_if<std::is_same<T, espurna::string>::value>::type>
bool delSetting(const T& key) {
    return delSettingString(key);
}
bool delSetting(const espurna::settings::Key& key);

void delSettingPrefix(espurna::settings::query::StringViewIterator);

bool hasSetting(const char* key);
bool hasSettingString(const espurna::string& key);

template <typename T, typename = typename std::enable_if<std::is_same<T, espurna::string>::value>::type>
bool hasSetting(const T& key) {
    return hasSettingString(key);
}
bool hasSetting(const espurna::settings::Key& key);

void settingsDump(const espurna::terminal::CommandContext&,
    const espurna::settings::query::Setting* begin,
    const espurna::settings::query::Setting* end);

template <typename T>
void settingsDump(const espurna::terminal::CommandContext& ctx, const T& settings) {
    settingsDump(ctx, std::begin(settings), std::end(settings));
}

void settingsDump(const espurna::terminal::CommandContext&,
    const espurna::settings::query::IndexedSetting* begin,
    const espurna::settings::query::IndexedSetting* end, size_t index);

template <typename T>
void settingsDump(const espurna::terminal::CommandContext& ctx, const T& settings, size_t index) {
    settingsDump(ctx, std::begin(settings), std::end(settings), index);
}

// {"app": ..., "version": ..., "backup": "1", "<key>": "<value>", ...}: the backup file (head)
void settingsPrintJson(espurna::Output&, bool pretty = false);

// the restore of that file (the ws module, settings_json.cpp)
namespace espurna {
namespace json {
class Value;
} // namespace json
} // namespace espurna

bool settingsRestoreJson(espurna::json::Value data);
void settingsJsonSetup();

size_t settingsKeyCount();
espurna::settings::Keys settingsKeys();

size_t settingsSize();

void settingsSetup();

// -----------------------------------------------------------------------------
// Configuration updates
// -----------------------------------------------------------------------------

using MigrateVersionCallback = void(*)(int);

void migrateVersion(MigrateVersionCallback);
int migrateVersion();
void migrate();

// -----------------------------------------------------------------------------
// Deprecated implementation
// -----------------------------------------------------------------------------

template<typename T>
espurna::string getSetting(const espurna::string& key, unsigned char index, T defaultValue)
__attribute__((deprecated("getSetting({key, index}, default) should be used instead")));

template<typename T>
bool setSetting(const espurna::string& key, unsigned char index, T value)
__attribute__((deprecated("setSetting({key, index}, value) should be used instead")));

template<typename T>
bool hasSetting(const espurna::string& key, unsigned char index)
__attribute__((deprecated("hasSetting({key, index}) should be used instead")));

template<typename T>
bool delSetting(const espurna::string& key, unsigned char index)
__attribute__((deprecated("delSetting({key, index}) should be used instead")));

// --------------------------------------------------------------------------

template<typename T>
espurna::string getSetting(const espurna::string& key, unsigned char index, T defaultValue) {
    return getSetting({key, index}, defaultValue);
}

template<typename T>
bool setSetting(const espurna::string& key, unsigned char index, T value) {
    return setSetting({key, index}, value);
}

template<typename T>
bool hasSetting(const espurna::string& key, unsigned char index) {
    return hasSetting({key, index});
}

template<typename T>
bool delSetting(const espurna::string& key, unsigned char index) {
    return delSetting({key, index});
}
