/*

JSON of ESPurna (owner 2026-11-18: instead of ArduinoJson, in the `tools` module, never in the head)

- writing: json::Writer goes straight into an espurna::Output (a WebSocket frame, an HTTP response, an MQTT
  payload), nothing is kept: no tree, no buffer of its own

      json::StreamWriter out(output);
      out.beginObject();
      out.field("relay", 1).field("name", "Kitchen");
      out.array("list").value(1).value(2).end();
      out.end();

- reading: json::Value is a view of the text it came from (nothing copied, no heap): object members by key, array
  items by index, each() over both. A broken text gives values that are not valid(), never a crash

      const auto root = json::parse(text);
      const auto id = root["id"].asLong();
      root["data"].members([](espurna::StringView key, json::Value value) { ... });

*/

#pragma once

#include "text_types.h"

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace json {

// The writer the code writes with: an interface, every call virtual. The head's parts write their WebUI members
// through it without a single symbol of the tools module (the head has no JSON code); StreamWriter below is the one
// implementation, in the tools module
class Writer {
public:
    virtual ~Writer() = default;

    virtual Writer& beginObject() = 0;
    virtual Writer& beginArray() = 0;
    // closes the last object or array
    virtual Writer& end() = 0;

    // the name of the next member (inside an object)
    virtual Writer& key(StringView name) = 0;

    virtual Writer& text(StringView value) = 0;
    virtual Writer& boolean(bool value) = 0;
    virtual Writer& integer(long long value) = 0;
    virtual Writer& unsignedInteger(unsigned long long value) = 0;
    virtual Writer& decimal(double value, unsigned char decimals) = 0;
    virtual Writer& nullValue() = 0;
    // already JSON (another writer's text, a value kept as text)
    virtual Writer& raw(StringView text) = 0;

    Writer& value(StringView value) {
        return text(value);
    }

    Writer& value(const char* value) {
        return value ? text(StringView(value)) : nullValue();
    }

    template <size_t Size>
    Writer& value(const char (&value)[Size]) {
        return text(StringView(value));
    }

    Writer& value(bool value) {
        return boolean(value);
    }

    Writer& value(int value) {
        return integer(value);
    }

    Writer& value(long value) {
        return integer(value);
    }

    Writer& value(long long value) {
        return integer(value);
    }

    Writer& value(unsigned char value) {
        return unsignedInteger(value);
    }

    Writer& value(unsigned short value) {
        return unsignedInteger(value);
    }

    Writer& value(short value) {
        return integer(value);
    }

    Writer& value(signed char value) {
        return integer(value);
    }

    Writer& value(unsigned value) {
        return unsignedInteger(value);
    }

    Writer& value(unsigned long value) {
        return unsignedInteger(value);
    }

    Writer& value(unsigned long long value) {
        return unsignedInteger(value);
    }

    Writer& value(double value, unsigned char decimals = 2) {
        return decimal(value, decimals);
    }

    Writer& value(float value, unsigned char decimals = 2) {
        return decimal(value, decimals);
    }

    Writer& value(std::nullptr_t) {
        return nullValue();
    }

    // espurna::string, espurna::string: anything with c_str() and length()
    template <typename T>
    auto value(const T& value) -> decltype(value.c_str(), value.length(), *this) {
        return text(StringView(value.c_str(), value.length()));
    }

    // key + value
    template <typename T>
    Writer& field(StringView name, const T& value) {
        key(name);
        return this->value(value);
    }

    Writer& field(StringView name, double value, unsigned char decimals) {
        key(name);
        return decimal(value, decimals);
    }

    // key + beginObject() / beginArray()
    Writer& object(StringView name) {
        key(name);
        return beginObject();
    }

    Writer& array(StringView name) {
        key(name);
        return beginArray();
    }
};

// JSON text straight into an espurna::Output: nothing kept, no tree, no buffer of its own (the tools module)
class StreamWriter final : public Writer {
public:
    explicit StreamWriter(Output& out) :
        _out(out)
    {}

    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    Writer& beginObject() override;
    Writer& beginArray() override;
    Writer& end() override;
    Writer& key(StringView name) override;
    Writer& text(StringView value) override;
    Writer& boolean(bool value) override;
    Writer& integer(long long value) override;
    Writer& unsignedInteger(unsigned long long value) override;
    Writer& decimal(double value, unsigned char decimals) override;
    Writer& nullValue() override;
    Writer& raw(StringView text) override;

    Output& output() {
        return _out;
    }

    // objects and arrays still open
    size_t depth() const {
        return _depth;
    }

private:
    void separate();
    void quoted(StringView value);

    Output& _out;
    uint32_t _members { 0 };   // bit per depth: something was written at that level already
    uint32_t _arrays { 0 };    // bit per depth: an array is open there
    uint8_t _depth { 0 };
    bool _keyed { false };     // a key was written, its value comes next
};

enum class Type : uint8_t {
    Invalid,
    Null,
    Bool,
    Number,
    String,
    Object,
    Array,
};

class Value {
public:
    Value() = default;

    // the whole value text: "\"text\"", "12", "{...}"
    explicit Value(StringView text);

    Type type() const {
        return _type;
    }

    bool valid() const {
        return _type != Type::Invalid;
    }

    bool isNull() const {
        return _type == Type::Null;
    }

    bool isBool() const {
        return _type == Type::Bool;
    }

    bool isNumber() const {
        return _type == Type::Number;
    }

    bool isString() const {
        return _type == Type::String;
    }

    bool isObject() const {
        return _type == Type::Object;
    }

    bool isArray() const {
        return _type == Type::Array;
    }

    // the value as it is in the text
    StringView raw() const {
        return _text;
    }

    // a number (or a string of one: "12"); `fallback` otherwise
    long asLong(long fallback = 0) const;
    unsigned long asULong(unsigned long fallback = 0) const;
    double asDouble(double fallback = 0.0) const;
    // true / false, a non-zero number, "1" / "true" / "on"
    bool asBool(bool fallback = false) const;

    // a string without its quotes and escapes; numbers and booleans as they are written. Empty otherwise
    string asString() const;
    // the same into `out` (terminated); the length, or 0 when it does not fit
    size_t asString(char* out, size_t size) const;

    // a string as a view of the text between its quotes (escapes left as they are: keys, names, settings values);
    // anything else as it is written
    StringView view() const {
        return (_type == Type::String)
            ? StringView(_text.begin() + 1, _text.end() - 1)
            : _text;
    }

    // the member of an object, the item of an array; invalid when none
    Value operator[](StringView key) const;
    Value operator[](const char* key) const {
        return (*this)[StringView(key)];
    }
    Value operator[](size_t index) const;
    Value operator[](int index) const {
        return (*this)[static_cast<size_t>(index)];
    }

    bool has(StringView key) const {
        return (*this)[key].valid();
    }

    // members of an object, items of an array
    size_t size() const;

    // object: callback(StringView key, Value value), the key as written (no escapes expected in keys).
    // Stops when the callback returns false (one returning void goes through all of them)
    template <typename T>
    void members(T&& callback) const;

    // array: callback(Value value), the same
    template <typename T>
    void items(T&& callback) const;

    class Iterator;

    // for (auto item : array) / for (auto member : object): an item is a Value, a member a Value too (its value) with
    // .key and .value
    Iterator begin() const;
    Iterator end() const;

private:
    friend class Iterator;

    // the next member / item after `ptr` (past '{' / '[' or a ','); false at the end
    bool next(const char*& ptr, StringView& key, Value& value) const;

    StringView _text;
    Type _type { Type::Invalid };
};

// an item of an array, a member of an object (key and value) in a range-for
struct Member : public Value {
    Member() = default;
    Member(StringView key, Value value) :
        Value(value),
        key(key),
        value(value)
    {}

    StringView key;
    Value value;
};

class Value::Iterator {
public:
    Iterator() = default;
    Iterator(const Value* owner, const char* ptr);

    const Member& operator*() const {
        return _current;
    }

    const Member* operator->() const {
        return &_current;
    }

    Iterator& operator++();

    bool operator!=(const Iterator& other) const {
        return _owner != other._owner;
    }

    bool operator==(const Iterator& other) const {
        return _owner == other._owner;
    }

private:
    void advance();

    const Value* _owner { nullptr };   // nullptr: the end
    const char* _ptr { nullptr };
    Member _current;
};

// the first value of `text` (whitespace around it ignored); invalid when it is not JSON
Value parse(StringView text);

namespace internal {

template <typename T, typename... Args>
bool call(void*, T& callback, Args&&... args) {
    callback(args...);
    return true;
}

template <typename T, typename... Args>
bool call(bool*, T& callback, Args&&... args) {
    return callback(args...);
}

} // namespace internal

template <typename T>
void Value::members(T&& callback) const {
    if (_type != Type::Object) {
        return;
    }

    const char* ptr = _text.begin() + 1;
    StringView key;
    Value value;
    while (next(ptr, key, value)) {
        if (!internal::call(static_cast<decltype(callback(key, value))*>(nullptr), callback, key, value)) {
            break;
        }
    }
}

template <typename T>
void Value::items(T&& callback) const {
    if (_type != Type::Array) {
        return;
    }

    const char* ptr = _text.begin() + 1;
    StringView key;
    Value value;
    while (next(ptr, key, value)) {
        if (!internal::call(static_cast<decltype(callback(value))*>(nullptr), callback, value)) {
            break;
        }
    }
}

} // namespace json
} // namespace espurna
