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
    /* 文件在这里 fopen，HTTP 在这里完成第一次 GET。失败返回 -1。 */
    virtual int open_io() { return 0; }
    virtual int read(std::uint8_t* dst, int size) = 0;
    virtual bool seekable() const = 0;
    virtual std::int64_t length() const = 0;
    /* whence matches SEEK_SET, SEEK_CUR, and SEEK_END. */
    virtual std::int64_t seek(std::int64_t offset, int whence) = 0;
    virtual NetTiming net_timing() const { return {}; }
    /* 只有 HttpSource 填过 DNS、建连、TLS、首字节。 */
    virtual bool has_net_timing() const { return false; }
    /* 用户 Seek 能不能做。缓冲里的光标移动不看这个值。 */
    virtual bool session_seekable() const { return seekable(); }
};

}  /* namespace legato */

#endif
