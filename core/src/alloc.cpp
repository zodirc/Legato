#include "legato/core/alloc.hpp"

#include <atomic>
#include <cstdlib>

namespace legato {
namespace {

std::atomic<std::uint64_t> g_steady_allocs{0};
std::atomic<bool> g_steady{false};

}  /* namespace */

void enter_steady_state() {
    g_steady_allocs.store(0, std::memory_order_relaxed);
    g_steady.store(true, std::memory_order_release);
}

void leave_steady_state() {
    g_steady.store(false, std::memory_order_release);
}

std::uint64_t steady_alloc_count() {
    return g_steady_allocs.load(std::memory_order_acquire);
}

void* tracked_malloc(std::size_t bytes) {
    if (g_steady.load(std::memory_order_acquire)) {
        g_steady_allocs.fetch_add(1, std::memory_order_relaxed);
    }
    if (bytes == 0) {
        bytes = 1;
    }
    return std::malloc(bytes);
}

void tracked_free(void* ptr) { std::free(ptr); }

}  /* namespace legato */
