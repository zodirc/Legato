#ifndef LEGATO_IO_CALLBACK_SOURCE_HPP
#define LEGATO_IO_CALLBACK_SOURCE_HPP

#include "legato/engine/engine.hpp"
#include "legato/io/data_source.hpp"

namespace legato {

/* 登记的 scheme 把读、定位和长度交给调用方。函数指针在打开时已经复制。
   userdata 必须活到会话释放，因为解复用线程还会调用它。 */
class CallbackSource : public DataSource {
public:
    explicit CallbackSource(const SourceRegistration& reg);

    int read(std::uint8_t* dst, int size) override;
    bool seekable() const override { return reg_.seek != nullptr; }
    std::int64_t length() const override;
    std::int64_t seek(std::int64_t offset, int whence) override;

private:
    SourceRegistration reg_;
};

}  /* namespace legato */

#endif
