#ifndef LEGATO_CORE_ACTOR_HPP
#define LEGATO_CORE_ACTOR_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace legato {

/* Serial executor. Inline mode runs the task inside post(). Threaded mode
   owns one thread and returns after the task is queued. */
class Actor {
public:
    explicit Actor(bool threaded);
    ~Actor();

    Actor(const Actor&) = delete;
    Actor& operator=(const Actor&) = delete;

    void post(std::function<void()> task);
    void drain();
    void stop();

private:
    void run();

    bool threaded_ = false;
    bool started_ = false;
    bool stopping_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
};

}  /* namespace legato */

#endif
