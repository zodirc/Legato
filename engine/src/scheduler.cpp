#include "legato/engine/scheduler.hpp"

namespace legato {

void Scheduler::schedule(std::uint64_t owner, std::int64_t when_us, Task task) {
    Item item;
    item.owner = owner;
    item.when_us = when_us;
    item.seq = seq_++;
    item.task = std::move(task);
    items_.push_back(std::move(item));
}

void Scheduler::cancel(std::uint64_t owner) {
    std::vector<Item> kept;
    kept.reserve(items_.size());
    for (auto& item : items_) {
        if (item.owner != owner) {
            kept.push_back(std::move(item));
        }
    }
    items_.swap(kept);
}

void Scheduler::advance_clock(std::int64_t delta_us) {
    if (delta_us < 0) {
        return;
    }
    now_ += delta_us;
}

bool Scheduler::take_due(Task* task, std::int64_t* when_us) {
    int best = -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[static_cast<size_t>(i)].when_us > now_) {
            continue;
        }
        if (best < 0 || items_[static_cast<size_t>(i)].when_us < items_[static_cast<size_t>(best)].when_us ||
            (items_[static_cast<size_t>(i)].when_us == items_[static_cast<size_t>(best)].when_us &&
             items_[static_cast<size_t>(i)].seq < items_[static_cast<size_t>(best)].seq)) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    Item item = std::move(items_[static_cast<size_t>(best)]);
    items_.erase(items_.begin() + best);
    if (when_us != nullptr) {
        *when_us = item.when_us;
    }
    if (task != nullptr) {
        *task = std::move(item.task);
    }
    return true;
}

void Scheduler::advance(std::int64_t delta_us) {
    advance_clock(delta_us);
    for (int guard = 0; guard < 100000; ++guard) {
        Task task;
        std::int64_t when_us = 0;
        if (!take_due(&task, &when_us)) {
            return;
        }
        if (task) {
            task(when_us);
        }
    }
}

}  /* namespace legato */
