#ifndef LEGATO_IO_HTTP_SOURCE_HPP
#define LEGATO_IO_HTTP_SOURCE_HPP

#include "legato/io/data_source.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace legato {

/* 播放用的 FFmpeg 关掉了网络。HTTP 由 libcurl 拉取，解复用只看见字节。
   这样协议白名单留在引擎里，不会绕过第 5.2 节。 */
class HttpSource : public DataSource {
public:
    explicit HttpSource(const char* url);
    ~HttpSource() override;

    int open_io() override;
    int read(std::uint8_t* dst, int size) override;
    bool seekable() const override { return true; }
    std::int64_t length() const override { return static_cast<std::int64_t>(body_.size()); }
    std::int64_t seek(std::int64_t offset, int whence) override;
    NetTiming net_timing() const override { return timing_; }
    bool has_net_timing() const override { return have_timing_; }
    bool session_seekable() const override { return session_seekable_; }

private:
    std::string url_;
    std::vector<std::uint8_t> body_;
    std::int64_t pos_ = 0;
    NetTiming timing_{};
    bool have_timing_ = false;
    bool session_seekable_ = false;
};

}  /* namespace legato */

#endif
