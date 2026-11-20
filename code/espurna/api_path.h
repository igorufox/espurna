/*

Part of the API MODULE

Copyright (C) 2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once

#include <vector>

#include "types.h"

// -----------------------------------------------------------------------------

struct PathPart {
    enum class Type {
        Unknown,
        Value,
        SingleWildcard,
        MultiWildcard
    };

    Type type;
    size_t offset;
    size_t length;
};

struct PathParts {
    using Parts = std::vector<PathPart>;

    PathParts() = delete;
    PathParts(const PathParts&) = delete;

    explicit PathParts(espurna::StringView path);
    PathParts(espurna::StringView path, Parts&& parts) :
        _path(path),
        _parts(std::move(parts)),
        _ok(_parts.size())
    {}

    PathParts(PathParts&& other) noexcept :
        PathParts(other._path, std::move(other._parts))
    {}

    PathParts(espurna::StringView path, PathParts&& other) noexcept :
        _path(path),
        _parts(std::move(other._parts)),
        _ok(other._ok)
    {}

    explicit operator bool() const {
        return _ok;
    }

    void clear() {
        _parts.clear();
    }

    void reserve(size_t size) {
        _parts.reserve(size);
    }

    espurna::StringView operator[](size_t index) const {
        return get(_parts[index]);
    }

    espurna::StringView back() const {
        return get(_parts.back());
    }

    espurna::StringView front() const {
        return get(_parts.front());
    }

    espurna::StringView path() const {
        return _path;
    }

    const Parts& parts() const {
        return _parts;
    }

    size_t size() const {
        return _parts.size();
    }

    Parts::const_iterator begin() const {
        return _parts.begin();
    }

    Parts::const_iterator end() const {
        return _parts.end();
    }

    bool match(const PathParts& path) const;
    bool match(espurna::StringView path) const {
        return match(PathParts(path));
    }

    static espurna::StringView wildcard(const PathParts& pattern, const PathParts& value, int index);
    static size_t wildcards(const PathParts& pattern);

private:
    espurna::StringView get(const PathPart& part) const {
        return espurna::StringView(
            _path.begin() + part.offset,
            _path.begin() + part.offset + part.length);
    }

    PathPart& emplace_back(PathPart part) {
        _parts.push_back(part);
        return _parts.back();
    }

    PathPart& emplace_back(PathPart::Type type, size_t offset, size_t length) {
        return emplace_back(PathPart{
            .type = type,
            .offset = offset,
            .length = length
        });
    }

    espurna::StringView _path;
    Parts _parts;
    bool _ok { false };
};

// Inline: every module that parses an API path (api, relay, light, mqtt, scheduler, sensors) has its own copy, the
// head has none (it parses no API path)

inline PathParts::PathParts(espurna::StringView path) :
    _path(path)
{
    if (!_path.length()) {
        _ok = false;
        return;
    }

    PathPart::Type type { PathPart::Type::Unknown };
    size_t length { 0ul };
    size_t offset { 0ul };

    const char* p { _path.begin() };
    if (*p == '\0') {
       goto error;
    }

    _parts.reserve(std::count(_path.begin(), _path.end(), '/') + 1);

start:
    type = PathPart::Type::Unknown;
    length = 0;
    offset = p - _path.c_str();

    switch (*p) {
    case '+':
        goto parse_single_wildcard;
    case '#':
        goto parse_multi_wildcard;
    case '/':
    default:
        goto parse_value;
    }

parse_value:
    type = PathPart::Type::Value;

    switch (*p) {
    case '+':
    case '#':
        goto error;
    case '/':
    case '\0':
        goto push_result;
    }

    ++p;
    ++length;

    goto parse_value;

parse_single_wildcard:
    type = PathPart::Type::SingleWildcard;

    ++p;
    switch (*p) {
    case '/':
        ++p;
    case '\0':
        goto push_result;
    }

    goto error;

parse_multi_wildcard:
    type = PathPart::Type::MultiWildcard;

    ++p;
    if (*p == '\0') {
        goto push_result;
    }
    goto error;

push_result:
    emplace_back(type, offset, length);
    if (*p == '/') {
        ++p;
        goto start;
    } else if (*p != '\0') {
        goto start;
    }
    goto success;

error:
    _ok = false;
    _parts.clear();
    return;

success:
    _ok = true;
}

// match when, for example, given the path 'topic/one/two/three' and pattern 'topic/+/two/+'

inline bool PathParts::match(const PathParts& path) const {
    if (!_ok || !path) {
        return false;
    }

    auto lhs = begin();
    auto lhs_end = end();

    auto rhs = path.begin();
    auto rhs_end = path.end();
loop:
    if (lhs == lhs_end) {
        goto check_end;
    }

    switch ((*lhs).type) {
    case PathPart::Type::Value:
        if (
            (rhs != rhs_end)
            && ((*rhs).type == PathPart::Type::Value)
            && ((*rhs).length == (*lhs).length)
        ) {
            if (0 == std::memcmp(
                _path.c_str() + (*lhs).offset,
                path.path().c_str() + (*rhs).offset,
                (*rhs).length))
            {
                std::advance(lhs, 1);
                std::advance(rhs, 1);
                goto loop;
            }
        }
        goto error;

    case PathPart::Type::SingleWildcard:
        if (
            (rhs != rhs_end)
            && ((*rhs).type == PathPart::Type::Value)
        ) {
            std::advance(lhs, 1);
            std::advance(rhs, 1);
            goto loop;
        }
        goto error;

    case PathPart::Type::MultiWildcard:
        if (std::next(lhs) == lhs_end) {
            while (rhs != rhs_end) {
                if ((*rhs).type != PathPart::Type::Value) {
                    goto error;
                }
                std::advance(rhs, 1);
            }
            lhs = lhs_end;
            break;
        }
        goto error;

    case PathPart::Type::Unknown:
        goto error;
    };

check_end:
    if ((lhs == lhs_end) && (rhs == rhs_end)) {
        return true;
    }

error:
    return false;
}

inline espurna::StringView PathParts::wildcard(const PathParts& pattern, const PathParts& value, int index) {
    if (index < 0) {
        index = std::abs(index + 1);
    }

    espurna::StringView out;

    if (std::abs(index) < pattern.parts().size()) {
        const auto& pattern_parts = pattern.parts();
        int counter { 0 };

        for (size_t part = 0; part < pattern.size(); ++part) {
            const auto& lhs = pattern_parts[part];
            const auto& rhs = value.parts()[part];

            const auto path = value.path();

            switch (lhs.type) {
            case PathPart::Type::Value:
            case PathPart::Type::Unknown:
                break;

            case PathPart::Type::SingleWildcard:
                if (counter == index) {
                    out = espurna::StringView(
                        path.begin() + rhs.offset, path.begin() + rhs.offset + rhs.length);
                    return out;
                }
                ++counter;
                break;

            case PathPart::Type::MultiWildcard:
                if (counter == index) {
                    out = espurna::StringView(
                        path.begin() + rhs.offset, path.end());
                }
                return out;
            }
        }
    }

    return out;
}

inline size_t PathParts::wildcards(const PathParts& pattern) {
    size_t out { 0 };

    for (const auto& part : pattern) {
        switch (part.type) {
        case PathPart::Type::Unknown:
        case PathPart::Type::Value:
        case PathPart::Type::MultiWildcard:
            break;
        case PathPart::Type::SingleWildcard:
            ++out;
            break;
        }
    }

    return out;
}
