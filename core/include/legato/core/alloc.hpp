#ifndef LEGATO_CORE_ALLOC_HPP
#define LEGATO_CORE_ALLOC_HPP

#include <cstddef>
#include <cstdint>

namespace legato {

/* 进入稳态后，tracked_malloc 和 liblegato 的 --wrap 都会加一。
   AllocExempt 深度大于 0 时不加。音频回调和播放节拍不得调用 tracked_malloc。 */
void enter_steady_state();
void leave_steady_state();
bool in_steady_state();
std::uint64_t steady_alloc_count();
void note_heap_alloc();
void* tracked_malloc(std::size_t bytes);
void tracked_free(void* ptr);

/* FFmpeg 在解复用和解码里仍会分配。这段里的堆分配不计入 alloc_steady，
   否则门禁恒大于 0。豁免的函数写在 idl/metrics.yaml。 */
class AllocExempt {
public:
    AllocExempt();
    ~AllocExempt();
    AllocExempt(const AllocExempt&) = delete;
    AllocExempt& operator=(const AllocExempt&) = delete;
};

}  /* namespace legato */

#endif
