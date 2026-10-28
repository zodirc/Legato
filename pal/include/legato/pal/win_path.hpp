#ifndef LEGATO_PAL_WIN_PATH_HPP
#define LEGATO_PAL_WIN_PATH_HPP

#include <cstddef>
#include <cstring>

namespace legato {

/* CreateFileW 在路径超过 260 个字符时要求 \\?\ 前缀，并且不再把 / 当成分隔符。
   只有盘符和 UNC 加前缀。相对路径加了前缀就打不开。
   1 是写出的路径，0 是空路径，-1 是放不进缓冲区。 */
inline int win32_extended_path(const char* path, char* out, std::size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }

    const char* prefix = "";
    const char* body = path;
    const bool extended = std::strncmp(path, "\\\\?\\", 4) == 0 || std::strncmp(path, "//?/", 4) == 0;
    if (extended) {
        prefix = "\\\\?\\";
        body = path + 4;
    } else {
        const bool slash0 = path[0] == '/' || path[0] == '\\';
        const bool slash1 = path[1] == '/' || path[1] == '\\';
        const unsigned char drive = static_cast<unsigned char>(path[0]);
        const bool alpha = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
        if (slash0 && slash1) {
            if (path[2] == '\0') {
                return 0;
            }
            prefix = "\\\\?\\UNC\\";
            body = path + 2;
        } else if (alpha && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
            prefix = "\\\\?\\";
            body = path;
        }
    }

    std::size_t used = 0;
    auto push = [&](char c) -> bool {
        if (used + 1 >= out_len) {
            return false;
        }
        out[used++] = c;
        return true;
    };
    for (const char* p = prefix; *p != '\0'; ++p) {
        if (!push(*p)) {
            return -1;
        }
    }
    for (const char* p = body; *p != '\0'; ++p) {
        const char c = *p == '/' ? '\\' : *p;
        if (!push(c)) {
            return -1;
        }
    }
    out[used] = '\0';
    return 1;
}

}  /* namespace legato */

#endif
