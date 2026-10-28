#ifndef LEGATO_ENGINE_URI_PATH_HPP
#define LEGATO_ENGINE_URI_PATH_HPP

#include "legato/legato_idl.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace legato {

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 只解码完整的 %HH。半截、非十六进制和 %00 原样留下，避免把 C 字符串截断。
   返回 1，缓冲区不够返回 -1。 */
inline int percent_decode(const char* in, char* out, std::size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return -1;
    }
    std::size_t used = 0;
    for (std::size_t i = 0; in[i] != '\0';) {
        if (in[i] == '%' && in[i + 1] != '\0' && in[i + 2] != '\0') {
            const int hi = hex_nibble(in[i + 1]);
            const int lo = hex_nibble(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                const int byte = (hi << 4) | lo;
                if (byte != 0) {
                    if (used + 1 >= out_len) {
                        return -1;
                    }
                    out[used++] = static_cast<char>(byte);
                    i += 3;
                    continue;
                }
            }
        }
        if (used + 1 >= out_len) {
            return -1;
        }
        out[used++] = in[i];
        ++i;
    }
    out[used] = '\0';
    return 1;
}

/* 1 是本地路径，0 不是，-1 是路径放不进缓冲区。
   file:// 先做百分号解码，再去掉 localhost，盘符形式去掉盘符前的斜杠。
   无 scheme 的路径不解码：% 是文件名的一部分。
   file://host/share 保留成 //host/share。POSIX 和 Windows 用同一套规则，避免把盘符解析成 /C:/。 */
inline int local_file_path(const char* uri, char* out, std::size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (uri == nullptr || std::strncmp(uri, "corpus:", 7) == 0) {
        return 0;
    }
    const char* path = uri;
    char decoded[LEGATO_URI_MAX];
    bool unc = false;
    if (std::strncmp(uri, "file://", 7) == 0) {
        if (percent_decode(uri + 7, decoded, sizeof(decoded)) < 0) {
            return -1;
        }
        path = decoded;
        if (std::strncmp(path, "localhost", 9) == 0) {
            path += 9;
        } else if (path[0] != '/' && path[0] != '\0') {
            unc = true;
        }
        if (!unc && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) != 0 && path[2] == ':') {
            ++path;
        }
    } else if (std::strstr(uri, "://") != nullptr) {
        return 0;
    }
    if (path[0] == '\0') {
        return 0;
    }
    const int wrote = unc ? std::snprintf(out, out_len, "//%s", path) : std::snprintf(out, out_len, "%s", path);
    if (wrote < 0 || static_cast<std::size_t>(wrote) >= out_len) {
        return -1;
    }
    return 1;
}

}  /* namespace legato */

#endif
