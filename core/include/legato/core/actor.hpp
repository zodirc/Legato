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

/* 测试在暂停后的静默窗口里数唤醒。窗口之外的调用不计。 */
void note_thread_wake();
void arm_quiet_window(std::int64_t delay_us);
std::uint64_t quiet_wake_count();

}  /* namespace legato */

#endif
