#ifndef LEGATO_CORE_JSON_MINI_HPP
#define LEGATO_CORE_JSON_MINI_HPP

#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

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

/* JSON 小数不读 LC_NUMERIC。printf 的浮点转换在 en_DK 一类区域设置里把小数点写成逗号，基线逐字比对会失败。
   先把值收成百万分之一的整数，再用 std::to_chars 写出数字，然后插入小数点。 */
inline bool json_fixed6(double value, char* out, std::size_t out_len) {
    if (out == nullptr || out_len == 0 || !std::isfinite(value)) {
        return false;
    }
    const double scaled = value * 1000000.0;
    if (scaled >= static_cast<double>(LLONG_MAX) || scaled <= static_cast<double>(LLONG_MIN)) {
        return false;
    }
    const long long micros = std::llround(scaled);
    if (micros == LLONG_MIN) {
        return false;
    }
    const bool negative = micros < 0;
    const unsigned long long magnitude =
        static_cast<unsigned long long>(negative ? -micros : micros);
    char raw[32];
    const std::to_chars_result wrote = std::to_chars(raw, raw + sizeof(raw), magnitude);
    if (wrote.ec != std::errc{}) {
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(wrote.ptr - raw);
    char padded[32];
    const char* digits = raw;
    std::size_t length = count;
    if (count < 6) {
        const std::size_t pad = 6 - count;
        std::memset(padded, '0', pad);
        std::memcpy(padded + pad, raw, count);
        digits = padded;
        length = 6;
    }
    const std::size_t whole = length - 6;
    const std::size_t need = (negative ? 1 : 0) + (whole == 0 ? 1 : whole) + 1 + 6 + 1;
    if (need > out_len) {
        return false;
    }
    char* cursor = out;
    if (negative) {
        *cursor++ = '-';
    }
    if (whole == 0) {
        *cursor++ = '0';
    } else {
        std::memcpy(cursor, digits, whole);
        cursor += whole;
    }
    *cursor++ = '.';
    std::memcpy(cursor, digits + whole, 6);
    cursor[6] = '\0';
    return true;
}

}  /* namespace legato */

#endif
