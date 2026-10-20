#include "legato/io/file_source.hpp"

#include <cstring>

namespace legato {

FileSource::FileSource(const char* path) : path_(path != nullptr ? path : "") {}

FileSource::~FileSource() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

int FileSource::open_file() {
    if (path_.empty()) {
        return -1;
    }
    file_ = std::fopen(path_.c_str(), "rb");
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
    if (fseeko(file_, static_cast<off_t>(offset), whence) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(ftello(file_));
}

}  /* namespace legato */
