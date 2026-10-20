#ifndef LEGATO_CORE_SPSC_RING_HPP
#define LEGATO_CORE_SPSC_RING_HPP

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace legato {

/* Single-producer single-consumer ring. Capacity is N items.
   Indices are monotonic counters so the buffer can be full. */
template <typename T, size_t N>
class SpscRing {
public:
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

    bool push(const T& value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == N) {
            return false;
        }
        slots_[tail & (N - 1)] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T* out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        *out = slots_[head & (N - 1)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return tail - head;
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T slots_[N]{};
};

}  /* namespace legato */

#endif
