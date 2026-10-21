#include "legato/engine/engine.hpp"

#include "legato/core/json_mini.hpp"
#include "legato/engine/player.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
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
    observe_ = std::thread([this] { observe_main(); });
}

void Engine::shutdown() {
    observe_stop_.store(true);
    if (observe_.joinable()) {
        observe_.join();
    }
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

void Engine::observe_main() {
    while (!observe_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (observe_stop_.load()) {
            return;
        }
        post([this] { poll_observers(); });
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
            std::vector<Scheduler::Task> due;
            std::vector<std::int64_t> whens;
            {
                std::lock_guard<std::mutex> lock(mu_);
                virtual_clock_.advance_us(step);
                scheduler_.advance_clock(step);
                Scheduler::Task task;
                std::int64_t when_us = 0;
                while (scheduler_.take_due(&task, &when_us)) {
                    due.push_back(std::move(task));
                    whens.push_back(when_us);
                }
            }
            for (std::size_t i = 0; i < due.size(); ++i) {
                if (due[i]) {
                    due[i](whens[i]);
                }
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

}  /* namespace legato */
