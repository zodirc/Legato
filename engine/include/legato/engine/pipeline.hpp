#ifndef LEGATO_ENGINE_PIPELINE_HPP
#define LEGATO_ENGINE_PIPELINE_HPP

#include <cstdint>

namespace legato {

class Pipeline {
public:
    virtual ~Pipeline() = default;
    virtual int open(const char* path) = 0;
    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void seek(std::int64_t position_us, std::uint32_t serial) = 0;
    virtual void stop() = 0;
};

}  /* namespace legato */

#endif
