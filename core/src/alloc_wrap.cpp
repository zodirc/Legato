#include "legato/core/alloc.hpp"

#include <cstddef>

/* 只在 liblegato 的链接行上 --wrap。__real_* 由链接器提供，不能放进不包 malloc 的静态库。 */
extern "C" {

void* __real_malloc(std::size_t size);
void* __real_calloc(std::size_t count, std::size_t size);
void* __real_realloc(void* ptr, std::size_t size);
int __real_posix_memalign(void** memptr, std::size_t alignment, std::size_t size);

void* __wrap_malloc(std::size_t size) {
    legato::note_heap_alloc();
    if (size == 0) {
        size = 1;
    }
    return __real_malloc(size);
}

void* __wrap_calloc(std::size_t count, std::size_t size) {
    legato::note_heap_alloc();
    return __real_calloc(count, size);
}

void* __wrap_realloc(void* ptr, std::size_t size) {
    if (size != 0) {
        legato::note_heap_alloc();
    }
    return __real_realloc(ptr, size);
}

int __wrap_posix_memalign(void** memptr, std::size_t alignment, std::size_t size) {
    /* 这份 FFmpeg 的 av_malloc 走 posix_memalign，不走 malloc。不包这一层就计不到解码分配。 */
    if (size != 0) {
        legato::note_heap_alloc();
    }
    return __real_posix_memalign(memptr, alignment, size);
}

}
