#include "legato/engine/engine.hpp"

#include "legato/core/json_mini.hpp"
#include "legato/engine/player.hpp"

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

void Engine::post_control(std::function<void()> task) {
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
}

std::shared_ptr<Engine> Engine::shared_default() {
    static std::shared_ptr<Engine> engine = std::make_shared<Engine>(nullptr);
    return engine;
}

void Engine::warmup() {
    std::lock_guard<std::mutex> lock(mu_);
    warmed_ = true;
}

void Engine::register_player(Player* player) { players_.push_back(player); }

void Engine::unregister_player(Player* player) {
    std::vector<Player*> kept;
    kept.reserve(players_.size());
    for (Player* item : players_) {
        if (item != player) {
            kept.push_back(item);
        }
    }
    players_.swap(kept);
}

int Engine::advance_us(std::int64_t delta_us) {
    if (delta_us < 0) {
        return -1;
    }
    std::vector<Player*> wake;
    post_control([&] {
        std::vector<Scheduler::Task> due;
        std::vector<std::int64_t> whens;
        {
            std::lock_guard<std::mutex> lock(mu_);
            scheduler_.advance_clock(delta_us);
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
        std::lock_guard<std::mutex> lock(mu_);
        for (Player* player : players_) {
            player->on_clock_advanced();
            if (player->take_wake()) {
                wake.push_back(player);
            }
        }
    });
    for (Player* player : wake) {
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
    post_control([&] {
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
            return;
        }
        std::snprintf(slot->scheme, sizeof(slot->scheme), "%s", scheme);
        slot->read = read;
        slot->seek = seek;
        slot->length = length;
        slot->userdata = userdata;
        slot->used = true;
        rc = 0;
    });
    return rc;
}

}  /* namespace legato */
