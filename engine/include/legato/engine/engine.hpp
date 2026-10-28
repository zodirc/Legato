#ifndef LEGATO_ENGINE_ENGINE_HPP
#define LEGATO_ENGINE_ENGINE_HPP

#include "legato/core/actor.hpp"
#include "legato/engine/clock.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace legato {

class Player;

struct SourceRegistration {
    char scheme[32]{};
    bool used = false;
    int (*read)(void* userdata, std::uint8_t* dst, int size) = nullptr;
    std::int64_t (*seek)(void* userdata, std::int64_t offset, int whence) = nullptr;
    std::int64_t (*length)(void* userdata) = nullptr;
    void* userdata = nullptr;
};

/* 锁顺序：Engine::mu_ 然后 Player::mu_。控制线程上不做阻塞 IO。
   post 入队后返回。post_and_wait 只给 legato_engine_advance_us 用。 */
class Engine {
public:
    friend class Player;

    explicit Engine(const char* config_json);
    ~Engine();

    static std::shared_ptr<Engine> shared_default();

    /* 在调用线程上停掉观测、回收和控制线程。不能在回收线程上调用，否则 join 自己会终止进程。 */
    void shutdown();
    bool headless() const { return headless_; }
    /* 确定性回归的清单根目录。来自创建参数 corpus_root，不编进动态库。 */
    const std::string& corpus_root() const { return corpus_root_; }
    VirtualClock& virtual_clock() { return virtual_clock_; }
    void warmup();
    int advance_us(std::int64_t delta_us);
    int register_source(const char* scheme, int (*read)(void*, std::uint8_t*, int),
                        std::int64_t (*seek)(void*, std::int64_t, int), std::int64_t (*length)(void*),
                        void* userdata);
    /* 复制登记项。调用方不要在持有 Player::mu_ 时调用，锁顺序是 Engine::mu_ 在前。 */
    bool copy_source(const char* scheme, SourceRegistration* out);
    void post(std::function<void()> task);
    void post_and_wait(std::function<void()> task);
    void post_reclaim(std::function<void()> task);
    /* 有 observe 时让观测线程按下一个到期点醒来。没有 observe 时它继续阻塞。 */
    void poke_observe();

private:
    void register_player(const std::shared_ptr<Player>& player);
    void unregister_player(Player* player);
    void poll_observers();
    void observe_main();

    std::mutex mu_;
    VirtualClock virtual_clock_;
    Actor control_{true};
    Actor reclaim_{true};
    std::thread::id control_id_{};
    std::thread observe_;
    std::atomic<bool> observe_stop_{false};
    std::mutex observe_mu_;
    std::condition_variable observe_cv_;
    std::uint64_t observe_gen_ = 0;
    std::vector<std::weak_ptr<Player>> players_;
    bool headless_ = true;
    bool warmed_ = false;
    std::string corpus_root_;
    SourceRegistration sources_[4]{};
};

}  /* namespace legato */

#endif
