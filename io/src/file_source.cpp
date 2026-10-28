#include "legato/io/file_source.hpp"

#include "legato/legato_idl.h"
#include "legato/pal/win_path.hpp"

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <windows.h>
#endif

namespace {
#if defined(_WIN32)
int file_seek(FILE* file, std::int64_t offset, int whence) { return _fseeki64(file, offset, whence); }

std::int64_t file_tell(FILE* file) { return _ftelli64(file); }

/* _wfopen 不接受 \\?\，超过 260 个字符的路径会失败。
   CreateFileW 打开后把句柄交给 CRT，读和 seek 仍用 FILE，避免两套光标。
   共享读写与 _wfopen 的 _SH_DENYNO 相同，播放时不独占文件。
   _open_osfhandle 成功后句柄归 fd，不能再 CloseHandle。 */
FILE* open_utf8(const char* path) {
    char extended[LEGATO_URI_MAX + 8];
    if (legato::win32_extended_path(path, extended, sizeof(extended)) != 1) {
        return nullptr;
    }
    wchar_t wide[LEGATO_URI_MAX + 8];
    const int wrote = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, extended, -1, wide,
                                           static_cast<int>(sizeof(wide) / sizeof(wide[0])));
    if (wrote <= 0) {
        return nullptr;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    FILE* file = _fdopen(fd, "rb");
    if (file == nullptr) {
        _close(fd);
        return nullptr;
    }
    return file;
}
#else
int file_seek(FILE* file, std::int64_t offset, int whence) {
    return fseeko(file, static_cast<off_t>(offset), whence);
}

std::int64_t file_tell(FILE* file) { return static_cast<std::int64_t>(ftello(file)); }

FILE* open_utf8(const char* path) { return std::fopen(path, "rb"); }
#endif
}  /* namespace */

namespace legato {

FileSource::FileSource(const char* path) : path_(path != nullptr ? path : "") {}

FileSource::~FileSource() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

int FileSource::open_io() { return open_file(); }

int FileSource::open_file() {
    if (path_.empty()) {
        return -1;
    }
    file_ = open_utf8(path_.c_str());
    if (file_ == nullptr) {
        return -1;
    }
    if (seek(0, SEEK_END) < 0) {
        return -1;
    }
    length_ = seek(0, SEEK_CUR);
    if (length_ < 0 || seek(0, SEEK_SET) < 0) {
        return -1;
    }
    return 0;
}

int FileSource::read(std::uint8_t* dst, int size) {
    if (file_ == nullptr || dst == nullptr || size <= 0) {
        return -1;
    }
    const std::size_t got = std::fread(dst, 1, static_cast<std::size_t>(size), file_);
    if (got == 0 && std::ferror(file_) != 0) {
        return -1;
    }
    return static_cast<int>(got);
}

std::int64_t FileSource::seek(std::int64_t offset, int whence) {
    if (file_ == nullptr) {
        return -1;
    }
    if (file_seek(file_, offset, whence) != 0) {
        return -1;
    }
    return file_tell(file_);
}

}  /* namespace legato */
