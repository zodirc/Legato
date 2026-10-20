#ifndef LEGATO_IO_DATA_SOURCE_HPP
#define LEGATO_IO_DATA_SOURCE_HPP

#include <cstdint>

namespace legato {

/* Byte source. Implementations: file, http, cache, push, app-provided. */
class DataSource {
public:
    struct NetTiming {
        std::int64_t dns_us = 0;
        std::int64_t connect_us = 0;
        std::int64_t tls_us = 0;
        std::int64_t first_byte_us = 0;
    };

    virtual ~DataSource() = default;
    virtual int read(std::uint8_t* dst, int size) = 0;
    virtual bool seekable() const = 0;
    virtual std::int64_t length() const = 0;
    /* whence matches SEEK_SET, SEEK_CUR, and SEEK_END. */
    virtual std::int64_t seek(std::int64_t offset, int whence) = 0;
    virtual NetTiming net_timing() const { return {}; }
};

}  /* namespace legato */

#endif
