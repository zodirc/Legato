#ifndef LEGATO_ENGINE_URI_PATH_HPP
#define LEGATO_ENGINE_URI_PATH_HPP

#include <cctype>
#include <cstdio>
#include <cstring>

namespace legato {

/* 1 是本地路径，0 不是，-1 是路径放不进缓冲区。
   file:///C:/a.mp4 去掉盘符前的斜杠；file://host/share 保留成 //host/share。
   POSIX 和 Windows 用同一套规则，避免把盘符解析成 /C:/。 */
inline int local_file_path(const char* uri, char* out, std::size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (uri == nullptr || std::strncmp(uri, "corpus:", 7) == 0) {
        return 0;
    }
    const char* path = uri;
    bool unc = false;
    if (std::strncmp(uri, "file://", 7) == 0) {
        path = uri + 7;
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
