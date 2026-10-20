#include "legato/core/actor.hpp"

namespace legato {

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
        task();
    }
}

}  /* namespace legato */
