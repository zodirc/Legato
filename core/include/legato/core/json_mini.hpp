#ifndef LEGATO_CORE_JSON_MINI_HPP
#define LEGATO_CORE_JSON_MINI_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace legato {

inline const char* json_skip_ws(const char* p) {
    while (p != nullptr && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        ++p;
    }
    return p;
}

/* Points begin/end at the raw value. Strings are returned without quotes. */
inline bool json_find_value(const char* json, const char* key, const char** begin, const char** end) {
    if (json == nullptr || key == nullptr || begin == nullptr || end == nullptr) {
        return false;
    }
    char pattern[128];
    if (std::strlen(key) > 100) {
        return false;
    }
    std::snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* hit = std::strstr(json, pattern);
    if (hit == nullptr) {
        return false;
    }
    const char* cursor = json_skip_ws(hit + std::strlen(pattern));
    if (*cursor != ':') {
        return false;
    }
    cursor = json_skip_ws(cursor + 1);
    if (*cursor == '"') {
        ++cursor;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '"') {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
            } else {
                ++cursor;
            }
        }
        if (*cursor != '"') {
            return false;
        }
        *begin = start;
        *end = cursor;
        return true;
    }
    const char* start = cursor;
    while (*cursor != '\0' && *cursor != ',' && *cursor != '}' && *cursor != ' ' && *cursor != '\n') {
        ++cursor;
    }
    if (start == cursor) {
        return false;
    }
    *begin = start;
    *end = cursor;
    return true;
}

inline bool json_copy_string(const char* json, const char* key, char* out, size_t out_len) {
    const char* begin = nullptr;
    const char* end = nullptr;
    if (!json_find_value(json, key, &begin, &end) || out == nullptr || out_len == 0) {
        return false;
    }
    size_t count = static_cast<size_t>(end - begin);
    if (count + 1 > out_len) {
        count = out_len - 1;
    }
    std::memcpy(out, begin, count);
    out[count] = '\0';
    return true;
}

inline bool json_copy_i64(const char* json, const char* key, int64_t* out) {
    const char* begin = nullptr;
    const char* end = nullptr;
    if (!json_find_value(json, key, &begin, &end) || out == nullptr) {
        return false;
    }
    char buf[64];
    size_t count = static_cast<size_t>(end - begin);
    if (count >= sizeof(buf)) {
        return false;
    }
    std::memcpy(buf, begin, count);
    buf[count] = '\0';
    char* parse_end = nullptr;
    long long value = std::strtoll(buf, &parse_end, 10);
    if (parse_end == buf) {
        return false;
    }
    *out = static_cast<int64_t>(value);
    return true;
}

}  /* namespace legato */

#endif
