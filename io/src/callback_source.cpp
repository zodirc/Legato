#include "legato/io/callback_source.hpp"

#include <cstdint>

namespace legato {

CallbackSource::CallbackSource(const SourceRegistration& reg) : reg_(reg) {}

int CallbackSource::read(std::uint8_t* dst, int size) {
    if (reg_.read == nullptr || dst == nullptr || size <= 0) {
        return -1;
    }
    return reg_.read(reg_.userdata, dst, size);
}

std::int64_t CallbackSource::length() const {
    if (reg_.length == nullptr) {
        return -1;
    }
    return reg_.length(reg_.userdata);
}

std::int64_t CallbackSource::seek(std::int64_t offset, int whence) {
    if (reg_.seek == nullptr) {
        return -1;
    }
    return reg_.seek(reg_.userdata, offset, whence);
}

}  /* namespace legato */
