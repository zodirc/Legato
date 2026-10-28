#include "legato/core/actor.hpp"

#include "legato/pal/clock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace legato {
namespace {

std::atomic<std::int64_t> g_quiet_after{INT64_C(0x7fffffffffffffff)};
std::atomic<std::uint64_t> g_quiet_wakes{0};
std::atomic<std::uint64_t> g_quiet_snapshot{0};
std::atomic<bool> g_quiet_frozen{false};
std::atomic<bool> g_quiet_armed{false};

}  /* namespace */

void note_thread_wake() {
    if (g_quiet_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    const std::int64_t after = g_quiet_after.load(std::memory_order_relaxed);
    if (monotonic_us() < after) {
        return;
    }
    g_quiet_wakes.fetch_add(1, std::memory_order_relaxed);
}

void arm_quiet_window(std::int64_t delay_us) {
    if (g_quiet_armed.exchange(true)) {
        return;
    }
    if (delay_us < 0) {
        delay_us = 0;
    }
    g_quiet_wakes.store(0);
    g_quiet_after.store(monotonic_us() + delay_us);
    const std::int64_t sleep_us = delay_us + 1000000;
    std::thread([sleep_us] {
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        g_quiet_snapshot.store(g_quiet_wakes.load());
        g_quiet_frozen.store(true);
    }).detach();
}

std::uint64_t quiet_wake_count() {
    if (g_quiet_frozen.load()) {
        return g_quiet_snapshot.load();
    }
    return g_quiet_wakes.load();
}

Actor::Actor(bool threaded) : threaded_(threaded) {
    if (!threaded_) {
        return;
    }
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

Actor::~Actor() { stop(); }

void Actor::post(std::function<void()> task) {
    if (!threaded_) {
        task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) {
            return;
        }
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void Actor::drain() {
    if (!threaded_) {
        return;
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return queue_.empty() || stopping_; });
}

void Actor::stop() {
    if (!started_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    started_ = false;
}

void Actor::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                cv_.notify_all();
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            if (queue_.empty()) {
                cv_.notify_all();
            }
        }
        note_thread_wake();
        task();
    }
}

}  /* namespace legato */
