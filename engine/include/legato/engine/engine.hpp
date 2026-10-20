#ifndef LEGATO_ENGINE_ENGINE_HPP
#define LEGATO_ENGINE_ENGINE_HPP

#include "legato/core/actor.hpp"
#include "legato/engine/scheduler.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

/* Lock order: Engine::mu_ then Player::mu_. The scheduler is only used on the
   control actor, and only while mu_ is held. Commands wait until that turn finishes. */
class Engine {
public:
    friend class Player;

    explicit Engine(const char* config_json);

    static std::shared_ptr<Engine> shared_default();

    bool headless() const { return headless_; }
    void warmup();
    int advance_us(std::int64_t delta_us);
    int register_source(const char* scheme, int (*read)(void*, std::uint8_t*, int),
                        std::int64_t (*seek)(void*, std::int64_t, int), std::int64_t (*length)(void*),
                        void* userdata);

private:
    void post_control(std::function<void()> task);
    void register_player(Player* player);
    void unregister_player(Player* player);

    std::mutex mu_;
    Scheduler scheduler_;
    Actor control_{true};
    std::thread::id control_id_{};
    std::vector<Player*> players_;
    bool headless_ = true;
    bool warmed_ = false;
    SourceRegistration sources_[4]{};
};

}  /* namespace legato */

#endif
