#include "legato/core/alloc.hpp"

#include <atomic>
#include <cstdlib>

namespace legato {
namespace {

std::atomic<std::uint64_t> g_steady_allocs{0};
std::atomic<bool> g_steady{false};
thread_local int g_in_tracked = 0;
thread_local int g_exempt_depth = 0;

}  /* namespace */

bool in_steady_state() { return g_steady.load(std::memory_order_acquire); }

void note_heap_alloc() {
    if (g_in_tracked != 0 || g_exempt_depth != 0) {
        return;
    }
    if (g_steady.load(std::memory_order_acquire)) {
        g_steady_allocs.fetch_add(1, std::memory_order_relaxed);
    }
}

AllocExempt::AllocExempt() { ++g_exempt_depth; }

AllocExempt::~AllocExempt() { --g_exempt_depth; }

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
    if (bytes == 0) {
        bytes = 1;
    }
    /* malloc 会被 --wrap 再计一次。这段里 note_heap_alloc 跳过，只在这里加一。 */
    g_in_tracked = 1;
    void* block = std::malloc(bytes);
    g_in_tracked = 0;
    if (g_steady.load(std::memory_order_acquire)) {
        g_steady_allocs.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}

void tracked_free(void* ptr) { std::free(ptr); }

}  /* namespace legato */
