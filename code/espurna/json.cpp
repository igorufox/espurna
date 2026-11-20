/*

JSON of ESPurna (json.h): the tools module

*/

#include "json.h"

// the tools module only (config/head.h): the head has no JSON code
#if MODULE_BUILD

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

namespace espurna {
namespace json {
namespace {

bool space(char c) {
    return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
}

const char* skipSpace(const char* ptr, const char* end) {
    while ((ptr < end) && space(*ptr)) {
        ++ptr;
    }

    return ptr;
}

// past the closing quote of the string that starts at `ptr` (the opening quote); nullptr when it does not end
const char* skipString(const char* ptr, const char* end) {
    for (++ptr; ptr < end; ++ptr) {
        if (*ptr == '\\') {
            ++ptr;
            continue;
        }

        if (*ptr == '"') {
            return ptr + 1;
        }
    }

    return nullptr;
}

// past the value that starts at `ptr`; nullptr when it is broken
const char* skipValue(const char* ptr, const char* end, Type& type) {
    if (ptr >= end) {
        return nullptr;
    }

    switch (*ptr) {
    case '"':
        type = Type::String;
        return skipString(ptr, end);

    case '{':
    case '[': {
        type = (*ptr == '{') ? Type::Object : Type::Array;
        size_t depth { 0 };
        for (; ptr < end; ++ptr) {
            switch (*ptr) {
            case '"':
                ptr = skipString(ptr, end);
                if (!ptr) {
                    return nullptr;
                }
                --ptr;
                break;
            case '{':
            case '[':
                ++depth;
                break;
            case '}':
            case ']':
                if (--depth == 0) {
                    return ptr + 1;
                }
                break;
            }
        }

        return nullptr;
    }

    case 't':
    case 'f':
    case 'n': {
        const StringView rest(ptr, end);
        if (rest.startsWith(STRING_VIEW("true"))) {
            type = Type::Bool;
            return ptr + 4;
        }
        if (rest.startsWith(STRING_VIEW("false"))) {
            type = Type::Bool;
            return ptr + 5;
        }
        if (rest.startsWith(STRING_VIEW("null"))) {
            type = Type::Null;
            return ptr + 4;
        }
        return nullptr;
    }
    }

    if ((*ptr == '-') || ((*ptr >= '0') && (*ptr <= '9'))) {
        type = Type::Number;
        ++ptr;
        while ((ptr < end) && (((*ptr >= '0') && (*ptr <= '9'))
            || (*ptr == '.') || (*ptr == 'e') || (*ptr == 'E') || (*ptr == '+') || (*ptr == '-')))
        {
            ++ptr;
        }
        return ptr;
    }

    return nullptr;
}

int hexDigit(char c) {
    return ((c >= '0') && (c <= '9')) ? (c - '0') :
        ((c >= 'a') && (c <= 'f')) ? (c - 'a' + 10) :
        ((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10) : -1;
}

// the string between the quotes of `text`, escapes resolved, into `out` (nullptr: only counted); the length
size_t unescape(StringView text, char* out, size_t size) {
    size_t length { 0 };
    const auto put = [&](char c) {
        if (out && (length < size)) {
            out[length] = c;
        }
        ++length;
    };

    const char* ptr = text.begin() + 1;
    const char* end = text.end() - 1;
    for (; ptr < end; ++ptr) {
        if (*ptr != '\\') {
            put(*ptr);
            continue;
        }

        if (++ptr >= end) {
            break;
        }

        switch (*ptr) {
        case 'n':
            put('\n');
            break;
        case 'r':
            put('\r');
            break;
        case 't':
            put('\t');
            break;
        case 'b':
            put('\b');
            break;
        case 'f':
            put('\f');
            break;
        case 'u': {
            uint32_t code { 0 };
            int digits { 0 };
            for (; (digits < 4) && ((ptr + 1) < end); ++digits) {
                const int digit = hexDigit(*(ptr + 1));
                if (digit < 0) {
                    break;
                }
                code = (code << 4) | static_cast<uint32_t>(digit);
                ++ptr;
            }

            // UTF-8 of the code point (no surrogate pairs: nothing ESPurna reads has them)
            if (code < 0x80) {
                put(static_cast<char>(code));
            } else if (code < 0x800) {
                put(static_cast<char>(0xc0 | (code >> 6)));
                put(static_cast<char>(0x80 | (code & 0x3f)));
            } else {
                put(static_cast<char>(0xe0 | (code >> 12)));
                put(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                put(static_cast<char>(0x80 | (code & 0x3f)));
            }
            break;
        }
        default:
            put(*ptr);
            break;
        }
    }

    return length;
}

} // namespace

// --- writer ----------------------------------------------------------------------------------------------------------

void StreamWriter::separate() {
    if (_keyed) {
        _keyed = false;
        return;
    }

    if (_depth && (_depth <= 32)) {
        const uint32_t bit = 1u << (_depth - 1);
        if (_members & bit) {
            _out.write(',');
        }
        _members |= bit;
    }
}

void StreamWriter::quoted(StringView value) {
    _out.write('"');

    const char* start = value.begin();
    for (const char* ptr = value.begin(); ptr != value.end(); ++ptr) {
        const auto c = static_cast<unsigned char>(*ptr);
        if ((c >= 0x20) && (c != '"') && (c != '\\')) {
            continue;
        }

        _out.write(start, ptr - start);
        start = ptr + 1;

        switch (c) {
        case '"':
            _out.write(STRING_VIEW("\\\""));
            break;
        case '\\':
            _out.write(STRING_VIEW("\\\\"));
            break;
        case '\n':
            _out.write(STRING_VIEW("\\n"));
            break;
        case '\r':
            _out.write(STRING_VIEW("\\r"));
            break;
        case '\t':
            _out.write(STRING_VIEW("\\t"));
            break;
        default:
            _out.printf("\\u%04x", static_cast<unsigned>(c));
            break;
        }
    }

    _out.write(start, value.end() - start);
    _out.write('"');
}

Writer& StreamWriter::beginObject() {
    separate();
    _out.write('{');
    ++_depth;
    if (_depth <= 32) {
        _members &= ~(1u << (_depth - 1));
    }
    return *this;
}

Writer& StreamWriter::beginArray() {
    separate();
    _out.write('[');
    ++_depth;
    if (_depth <= 32) {
        _members &= ~(1u << (_depth - 1));
        _arrays |= 1u << (_depth - 1);
    }
    return *this;
}

// the closing bracket of what is open: the writer does not remember which one, the bit above the members' says it
Writer& StreamWriter::end() {
    if (!_depth) {
        return *this;
    }

    const uint32_t bit = (_depth <= 32) ? (1u << (_depth - 1)) : 0;
    _out.write((_arrays & bit) ? ']' : '}');
    _arrays &= ~bit;
    --_depth;
    _keyed = false;
    return *this;
}

Writer& StreamWriter::key(StringView name) {
    separate();
    quoted(name);
    _out.write(':');
    _keyed = true;
    return *this;
}

Writer& StreamWriter::text(StringView value) {
    separate();
    quoted(value);
    return *this;
}

Writer& StreamWriter::boolean(bool value) {
    separate();
    _out.write(value ? STRING_VIEW("true") : STRING_VIEW("false"));
    return *this;
}

Writer& StreamWriter::integer(long long value) {
    separate();
    if ((value >= -2147483647LL) && (value <= 2147483647LL)) {
        _out.printf("%ld", static_cast<long>(value));
    } else {
        _out.printf("%lld", value);
    }
    return *this;
}

Writer& StreamWriter::unsignedInteger(unsigned long long value) {
    separate();
    if (value <= 4294967295ULL) {
        _out.printf("%lu", static_cast<unsigned long>(value));
    } else {
        _out.printf("%llu", value);
    }
    return *this;
}

Writer& StreamWriter::decimal(double value, unsigned char decimals) {
    separate();
    char buffer[24];
    const auto length = formatDecimal(value, decimals, buffer, sizeof(buffer));
    if ((buffer[0] == 'n') || (buffer[0] == 'i') || ((buffer[0] == '-') && (buffer[1] == 'i'))) {
        _out.write(STRING_VIEW("null"));
    } else {
        _out.write(buffer, length);
    }
    return *this;
}

Writer& StreamWriter::nullValue() {
    separate();
    _out.write(STRING_VIEW("null"));
    return *this;
}

Writer& StreamWriter::raw(StringView text) {
    separate();
    _out.write(text);
    return *this;
}

// --- reader ----------------------------------------------------------------------------------------------------------

Value::Value(StringView text) {
    Type type { Type::Invalid };
    const auto* end = skipValue(text.begin(), text.end(), type);
    if (end) {
        _text = StringView(text.begin(), end);
        _type = type;
    }
}

Value parse(StringView text) {
    const char* begin = skipSpace(text.begin(), text.end());
    return Value(StringView(begin, text.end()));
}

bool Value::next(const char*& ptr, StringView& key, Value& value) const {
    const char* end = _text.end() - 1;
    ptr = skipSpace(ptr, end);
    if ((ptr < end) && (*ptr == ',')) {
        ptr = skipSpace(ptr + 1, end);
    }

    if (ptr >= end) {
        return false;
    }

    if (_type == Type::Object) {
        if (*ptr != '"') {
            return false;
        }

        const char* key_end = skipString(ptr, end);
        if (!key_end) {
            return false;
        }

        key = StringView(ptr + 1, key_end - 1);
        ptr = skipSpace(key_end, end);
        if ((ptr >= end) || (*ptr != ':')) {
            return false;
        }

        ptr = skipSpace(ptr + 1, end);
    }

    value = Value(StringView(ptr, end));
    if (!value.valid()) {
        return false;
    }

    ptr = value._text.end();
    return true;
}

Value Value::operator[](StringView key) const {
    Value out;
    members([&](StringView name, Value value) {
        if (name == key) {
            out = value;
            return false;
        }
        return true;
    });

    return out;
}

Value Value::operator[](size_t index) const {
    Value out;
    size_t current { 0 };
    items([&](Value value) {
        if (current++ == index) {
            out = value;
            return false;
        }
        return true;
    });

    return out;
}

Value::Iterator::Iterator(const Value* owner, const char* ptr) :
    _owner(owner),
    _ptr(ptr)
{
    advance();
}

void Value::Iterator::advance() {
    if (!_owner) {
        return;
    }

    StringView key;
    Value value;
    if (!_owner->next(_ptr, key, value)) {
        _owner = nullptr;
        return;
    }

    _current = Member(key, value);
}

Value::Iterator& Value::Iterator::operator++() {
    advance();
    return *this;
}

Value::Iterator Value::begin() const {
    if ((_type != Type::Object) && (_type != Type::Array)) {
        return Iterator();
    }

    return Iterator(this, _text.begin() + 1);
}

Value::Iterator Value::end() const {
    return Iterator();
}

size_t Value::size() const {
    size_t out { 0 };
    if (_type == Type::Object) {
        members([&](StringView, Value) {
            ++out;
        });
    } else if (_type == Type::Array) {
        items([&](Value) {
            ++out;
        });
    }

    return out;
}

size_t Value::asString(char* out, size_t size) const {
    if (!size) {
        return 0;
    }

    size_t length { 0 };
    switch (_type) {
    case Type::String:
        length = unescape(_text, out, size);
        break;
    case Type::Number:
    case Type::Bool:
        length = _text.length();
        if (length < size) {
            std::memcpy(out, _text.data(), length);
        }
        break;
    default:
        break;
    }

    if (length >= size) {
        out[0] = '\0';
        return 0;
    }

    out[length] = '\0';
    return length;
}

string Value::asString() const {
    switch (_type) {
    case Type::String: {
        const auto length = unescape(_text, nullptr, 0);
        if (length == (_text.length() - 2)) {
            return string(view());
        }

        string out;
        char buffer[64];
        if (length < sizeof(buffer)) {
            unescape(_text, buffer, sizeof(buffer));
            return string(buffer, length);
        }

        auto* data = new (std::nothrow) char[length];
        if (data) {
            unescape(_text, data, length);
            out = string(data, length);
            delete[] data;
        }

        return out;
    }

    case Type::Number:
    case Type::Bool:
        return string(_text);

    default:
        break;
    }

    return string();
}

double Value::asDouble(double fallback) const {
    const char* end { nullptr };
    if (_type == Type::Number) {
        return parseDecimal(_text.begin(), &end);
    }

    if (_type == Type::String) {
        char buffer[32];
        if (asString(buffer, sizeof(buffer))) {
            const auto out = parseDecimal(buffer, &end);
            if (end != buffer) {
                return out;
            }
        }
    }

    if (_type == Type::Bool) {
        return (_text.length() == 4) ? 1.0 : 0.0;
    }

    return fallback;
}

long Value::asLong(long fallback) const {
    if ((_type == Type::Number) || (_type == Type::String)) {
        char buffer[32];
        if (asString(buffer, sizeof(buffer))) {
            char* end { nullptr };
            const auto out = std::strtol(buffer, &end, 10);
            if (end != buffer) {
                return out;
            }
        }
    }

    if (_type == Type::Bool) {
        return (_text.length() == 4) ? 1 : 0;
    }

    return fallback;
}

unsigned long Value::asULong(unsigned long fallback) const {
    if ((_type == Type::Number) || (_type == Type::String)) {
        char buffer[32];
        if (asString(buffer, sizeof(buffer))) {
            char* end { nullptr };
            const auto out = std::strtoul(buffer, &end, 10);
            if (end != buffer) {
                return out;
            }
        }
    }

    if (_type == Type::Bool) {
        return (_text.length() == 4) ? 1 : 0;
    }

    return fallback;
}

bool Value::asBool(bool fallback) const {
    switch (_type) {
    case Type::Bool:
        return _text.length() == 4;
    case Type::Number:
        return asDouble() != 0.0;
    case Type::String: {
        const auto text = view();
        return (text == STRING_VIEW("1")) || (text == STRING_VIEW("true")) || (text == STRING_VIEW("on"));
    }
    default:
        break;
    }

    return fallback;
}

} // namespace json
} // namespace espurna

#endif // MODULE_BUILD
