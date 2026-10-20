#ifndef LEGATO_ENGINE_SCHEDULER_HPP
#define LEGATO_ENGINE_SCHEDULER_HPP

#include <cstdint>
#include <functional>
#include <vector>

namespace legato {

/* Virtual clock for headless runs. Tasks fire in time order when advance()
   jumps the clock. Not thread-safe; the engine mutex covers it. */
class Scheduler {
public:
    using Task = std::function<void(std::int64_t when_us)>;

    std::int64_t now() const { return now_; }

    void schedule(std::uint64_t owner, std::int64_t when_us, Task task);
    void cancel(std::uint64_t owner);
    /* Move the clock without running tasks. */
    void advance_clock(std::int64_t delta_us);
    /* Pop the earliest task that is due at now(). */
    bool take_due(Task* task, std::int64_t* when_us);
    void advance(std::int64_t delta_us);

private:
    struct Item {
        std::uint64_t owner = 0;
        std::int64_t when_us = 0;
        std::uint64_t seq = 0;
        Task task;
    };

    std::int64_t now_ = 0;
    std::uint64_t seq_ = 0;
    std::vector<Item> items_;
};

}  /* namespace legato */

#endif
