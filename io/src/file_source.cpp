#include "legato/io/file_source.hpp"

#include "legato/legato_idl.h"

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
#if defined(_WIN32)
int file_seek(FILE* file, std::int64_t offset, int whence) { return _fseeki64(file, offset, whence); }

std::int64_t file_tell(FILE* file) { return _ftelli64(file); }

/* CRT 的 fopen 按 ANSI 代码页解释路径。C ABI 传入的是 UTF-8，这里转成 UTF-16。 */
FILE* open_utf8(const char* path) {
    wchar_t wide[LEGATO_URI_MAX];
    const int wrote = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, LEGATO_URI_MAX);
    if (wrote <= 0) {
        return nullptr;
    }
    return _wfopen(wide, L"rb");
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
