#ifndef LEGATO_CORE_ALLOC_HPP
#define LEGATO_CORE_ALLOC_HPP

#include <cstddef>
#include <cstdint>

namespace legato {

/* Steady-state counter. enter_steady_state() zeros the count.
   tracked_malloc during steady increments it. The audio callback and the
   playing tick must not call tracked_malloc. */
void enter_steady_state();
void leave_steady_state();
std::uint64_t steady_alloc_count();
void* tracked_malloc(std::size_t bytes);
void tracked_free(void* ptr);

}  /* namespace legato */

#endif
