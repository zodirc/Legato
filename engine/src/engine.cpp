#include "legato/engine/engine.hpp"

#include "legato/core/json_mini.hpp"
#include "legato/engine/player.hpp"

#include "legato/engine/test_hooks.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <utility>

namespace legato {
namespace {

struct ControlWait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;

    void finish() {
        std::lock_guard<std::mutex> lock(mu);
        done = true;
        cv.notify_one();
    }
};

}  /* namespace */

void Engine::post(std::function<void()> task) {
    if (std::this_thread::get_id() == control_id_) {
        task();
        return;
    }
    control_.post(std::move(task));
}

void Engine::post_and_wait(std::function<void()> task) {
    if (std::this_thread::get_id() == control_id_) {
        task();
        return;
    }
    auto wait = std::make_shared<ControlWait>();
    control_.post([wait, task = std::move(task)]() mutable {
        struct Finish {
            std::shared_ptr<ControlWait> wait;
            ~Finish() {
                if (wait) {
                    wait->finish();
                }
            }
        } finish{wait};
        task();
    });
    std::unique_lock<std::mutex> lock(wait->mu);
    wait->cv.wait(lock, [&] { return wait->done; });
}

void Engine::post_reclaim(std::function<void()> task) { reclaim_.post(std::move(task)); }

Engine::Engine(const char* config_json) {
    std::promise<void> ready;
    control_.post([this, &ready] {
        control_id_ = std::this_thread::get_id();
        ready.set_value();
    });
    ready.get_future().wait();
    char mode[32];
    if (json_copy_string(config_json, "mode", mode, sizeof(mode))) {
        headless_ = std::strcmp(mode, "system") != 0;
    }
    const char* root_begin = nullptr;
    const char* root_end = nullptr;
    if (json_find_value(config_json, "corpus_root", &root_begin, &root_end) && root_end > root_begin) {
        corpus_root_.assign(root_begin, root_end);
    }
    observe_ = std::thread([this] { observe_main(); });
    const char* quiet = std::getenv("LEGATO_TEST_QUIET");
    if (quiet != nullptr && std::strcmp(quiet, "idle") == 0) {
        arm_quiet_window(200000);
    }
}

void Engine::shutdown() {
    observe_stop_.store(true);
    poke_observe();
    if (observe_.joinable()) {
        observe_.join();
    }
    log_test_quiet();
    reclaim_.stop();
    control_.stop();
}

Engine::~Engine() { shutdown(); }

std::shared_ptr<Engine> Engine::shared_default() {
    static std::shared_ptr<Engine> engine = std::make_shared<Engine>(nullptr);
    return engine;
}

void Engine::warmup() {
    std::lock_guard<std::mutex> lock(mu_);
    warmed_ = true;
}

void Engine::register_player(const std::shared_ptr<Player>& player) { players_.push_back(player); }

void Engine::unregister_player(Player* player) {
    std::vector<std::weak_ptr<Player>> kept;
    kept.reserve(players_.size());
    for (const std::weak_ptr<Player>& item : players_) {
        std::shared_ptr<Player> live = item.lock();
        if (live && live.get() != player) {
            kept.push_back(item);
        }
    }
    players_.swap(kept);
}

void Engine::poke_observe() {
    {
        std::lock_guard<std::mutex> lock(observe_mu_);
        ++observe_gen_;
    }
    observe_cv_.notify_all();
}

void Engine::observe_main() {
    std::uint64_t seen = 0;
    while (!observe_stop_.load()) {
        std::int64_t delay_us = -1;
        std::vector<std::shared_ptr<Player>> live;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const std::weak_ptr<Player>& item : players_) {
                if (std::shared_ptr<Player> player = item.lock()) {
                    live.push_back(std::move(player));
                }
            }
        }
        for (const std::shared_ptr<Player>& player : live) {
            const std::int64_t delay = player->claim_observe_delay_us();
            if (delay < 0) {
                continue;
            }
            if (delay_us < 0 || delay < delay_us) {
                delay_us = delay;
            }
        }
        if (delay_us == 0) {
            note_thread_wake();
            post([this] { poll_observers(); });
            continue;
        }
        std::unique_lock<std::mutex> lock(observe_mu_);
        if (observe_stop_.load()) {
            return;
        }
        if (observe_gen_ != seen) {
            seen = observe_gen_;
            continue;
        }
        if (delay_us < 0) {
            observe_cv_.wait(lock, [this, seen] { return observe_stop_.load() || observe_gen_ != seen; });
        } else {
            observe_cv_.wait_for(lock, std::chrono::microseconds(delay_us),
                                 [this, seen] { return observe_stop_.load() || observe_gen_ != seen; });
        }
        if (observe_stop_.load()) {
            return;
        }
        seen = observe_gen_;
    }
}

void Engine::poll_observers() {
    std::vector<std::shared_ptr<Player>> live;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const std::weak_ptr<Player>& item : players_) {
            if (std::shared_ptr<Player> player = item.lock()) {
                live.push_back(std::move(player));
            }
        }
    }
    for (const std::shared_ptr<Player>& player : live) {
        player->poll_observe();
    }
}

int Engine::advance_us(std::int64_t delta_us) {
    if (delta_us < 0) {
        return -1;
    }
    std::vector<std::shared_ptr<Player>> live;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const std::weak_ptr<Player>& item : players_) {
            if (std::shared_ptr<Player> player = item.lock()) {
                live.push_back(std::move(player));
            }
        }
    }
    for (const std::shared_ptr<Player>& player : live) {
        player->prepare_advance(delta_us);
    }
    std::vector<std::shared_ptr<Player>> wake;
    post_and_wait([&] {
        constexpr std::int64_t kVsyncUs = 16667;
        std::int64_t left = delta_us;
        while (left > 0) {
            const std::int64_t step = left > kVsyncUs ? kVsyncUs : left;
            {
                std::lock_guard<std::mutex> lock(mu_);
                virtual_clock_.advance_us(step);
            }
            left -= step;
        }
        std::vector<std::shared_ptr<Player>> due_players;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const std::weak_ptr<Player>& item : players_) {
                if (std::shared_ptr<Player> player = item.lock()) {
                    due_players.push_back(std::move(player));
                }
            }
        }
        for (const std::shared_ptr<Player>& player : due_players) {
            player->on_clock_advanced();
            if (player->take_wake()) {
                wake.push_back(player);
            }
        }
    });
    for (const std::shared_ptr<Player>& player : wake) {
        player->invoke_wakeup();
    }
    return 0;
}

int Engine::register_source(const char* scheme, int (*read)(void*, std::uint8_t*, int),
                            std::int64_t (*seek)(void*, std::int64_t, int), std::int64_t (*length)(void*),
                            void* userdata) {
    if (scheme == nullptr || scheme[0] == '\0' || read == nullptr || std::strlen(scheme) >= 32) {
        return -1;
    }
    int rc = -1;
    {
        std::lock_guard<std::mutex> lock(mu_);
        SourceRegistration* slot = nullptr;
        for (auto& item : sources_) {
            if (item.used && std::strcmp(item.scheme, scheme) == 0) {
                slot = &item;
                break;
            }
        }
        if (slot == nullptr) {
            for (auto& item : sources_) {
                if (!item.used) {
                    slot = &item;
                    break;
                }
            }
        }
        if (slot == nullptr) {
            return -1;
        }
        std::snprintf(slot->scheme, sizeof(slot->scheme), "%s", scheme);
        slot->read = read;
        slot->seek = seek;
        slot->length = length;
        slot->userdata = userdata;
        slot->used = true;
        rc = 0;
    }
    return rc;
}

bool Engine::copy_source(const char* scheme, SourceRegistration* out) {
    if (scheme == nullptr || out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& item : sources_) {
        if (item.used && std::strcmp(item.scheme, scheme) == 0) {
            *out = item;
            return true;
        }
    }
    return false;
}

}  /* namespace legato */
