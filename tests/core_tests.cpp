#include "legato/core/actor.hpp"
#include "legato/core/alloc.hpp"
#include "legato/core/object_pool.hpp"
#include "legato/core/spsc_ring.hpp"
#include "legato/codec/decoder.hpp"
#include "legato/demux/demuxer.hpp"
#include "legato/engine/preset.hpp"
#include "legato/engine/scheduler.hpp"
#include "legato/io/data_source.hpp"
#include "legato/pal/clock.hpp"
#include "legato/render/sink.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d %s\n", "core_tests", line, expr);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void test_spsc() {
    legato::SpscRing<int, 1024> ring;
    std::thread producer([&ring] {
        for (int i = 0; i < 10000; ++i) {
            while (!ring.push(i)) {
            }
        }
    });
    int expect = 0;
    int got = 0;
    while (got < 10000) {
        int value = -1;
        if (ring.pop(&value)) {
            CHECK(value == expect);
            ++expect;
            ++got;
        }
    }
    producer.join();
    CHECK(ring.size() == 0);
}

void test_pool() {
    legato::ObjectPool<int, 4> pool;
    int* slots[4] = {};
    for (int i = 0; i < 4; ++i) {
        slots[i] = pool.acquire();
        CHECK(slots[i] != nullptr);
        *slots[i] = i;
    }
    CHECK(pool.acquire() == nullptr);
    pool.release(slots[2]);
    int* again = pool.acquire();
    CHECK(again == slots[2]);
}

void test_actor() {
    legato::Actor inline_actor(false);
    int value = 0;
    inline_actor.post([&value] { value = 7; });
    CHECK(value == 7);

    legato::Actor threaded(true);
    std::atomic<int> counter{0};
    for (int i = 0; i < 1000; ++i) {
        threaded.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    threaded.stop();
    CHECK(counter.load() == 1000);
}

void test_scheduler() {
    legato::Scheduler scheduler;
    std::vector<int> order;
    scheduler.schedule(1, 30, [&order](std::int64_t) { order.push_back(30); });
    scheduler.schedule(1, 10, [&order](std::int64_t) { order.push_back(10); });
    scheduler.schedule(2, 20, [&order](std::int64_t) { order.push_back(20); });
    scheduler.advance(100);
    CHECK(order.size() == 3);
    CHECK(order.size() == 3 && order[0] == 10 && order[1] == 20 && order[2] == 30);
    CHECK(scheduler.now() == 100);

    scheduler.schedule(7, 150, [&order](std::int64_t) { order.push_back(7); });
    scheduler.schedule(8, 160, [&order](std::int64_t) { order.push_back(8); });
    scheduler.cancel(7);
    scheduler.advance(100);
    CHECK(order.size() == 4 && order[3] == 8);
    CHECK(scheduler.now() == 200);
}

void test_alloc() {
    legato::leave_steady_state();
    void* block = legato::tracked_malloc(16);
    legato::tracked_free(block);
    CHECK(legato::steady_alloc_count() == 0);
    legato::enter_steady_state();
    block = legato::tracked_malloc(16);
    CHECK(legato::steady_alloc_count() == 1);
    legato::tracked_free(block);
    legato::leave_steady_state();
}

void test_clock_and_presets() {
    const std::int64_t first = legato::monotonic_us();
    const std::int64_t second = legato::monotonic_us();
    CHECK(first > 0);
    CHECK(second >= first);
    const legato::PresetDesc* vod = legato::find_preset("vod");
    CHECK(vod != nullptr && vod->buffer_max_s == 50.0);
    CHECK(legato::find_preset("short_video")->preload_ahead == 2);
    CHECK(legato::find_preset("missing") == nullptr);
    CHECK(std::strcmp(legato::default_preset()->name, "vod") == 0);
}

}  /* namespace */

int main() {
    test_spsc();
    test_pool();
    test_actor();
    test_scheduler();
    test_alloc();
    test_clock_and_presets();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
