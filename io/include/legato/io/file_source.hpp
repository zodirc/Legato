#ifndef LEGATO_IO_FILE_SOURCE_HPP
#define LEGATO_IO_FILE_SOURCE_HPP

#include "legato/io/data_source.hpp"

#include <cstdio>
#include <string>

namespace legato {

class FileSource : public DataSource {
public:
    explicit FileSource(const char* path);
    ~FileSource() override;

    int open_io() override;
    int open_file();
    int read(std::uint8_t* dst, int size) override;
    bool seekable() const override { return file_ != nullptr; }
    std::int64_t length() const override { return length_; }
    std::int64_t seek(std::int64_t offset, int whence) override;

private:
    std::string path_;
    FILE* file_ = nullptr;
    std::int64_t length_ = -1;
};

}  /* namespace legato */

#endif
