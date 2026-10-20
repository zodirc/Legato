#ifndef LEGATO_CORE_OBJECT_POOL_HPP
#define LEGATO_CORE_OBJECT_POOL_HPP

#include <cstddef>
#include <cstdint>

namespace legato {

/* Fixed pool. acquire/release do not touch the heap. */
template <typename T, size_t N>
class ObjectPool {
public:
    ObjectPool() {
        for (size_t i = 0; i < N; ++i) {
            next_[i] = static_cast<int>(i + 1);
        }
        next_[N - 1] = -1;
        free_ = 0;
    }

    T* acquire() {
        if (free_ < 0) {
            return nullptr;
        }
        const int index = free_;
        free_ = next_[index];
        return &items_[index];
    }

    void release(T* item) {
        const auto index = static_cast<int>(item - items_);
        next_[index] = free_;
        free_ = index;
    }

    size_t capacity() const { return N; }

private:
    T items_[N]{};
    int next_[N]{};
    int free_ = -1;
};

}  /* namespace legato */

#endif
