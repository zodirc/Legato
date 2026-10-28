#include "legato/core/actor.hpp"
#include "legato/core/alloc.hpp"
#include "legato/core/json_mini.hpp"
#include "legato/core/object_pool.hpp"
#include "legato/core/spsc_ring.hpp"
#include "legato/codec/decoder.hpp"
#include "legato/demux/demuxer.hpp"
#include "legato/engine/error_info.hpp"
#include "legato/engine/uri_path.hpp"
#include "legato/engine/preset.hpp"
#include "legato/io/data_source.hpp"
#include "legato/pal/clock.hpp"
#include "legato/pal/win_path.hpp"
#include "legato/render/sink.hpp"

#include <atomic>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

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
    CHECK(vod->exact_seek);
    CHECK(vod->video);
    CHECK(legato::find_preset("short_video")->exact_seek == false);
    CHECK(legato::find_preset("music")->video == false);
    CHECK(legato::find_preset("short_video")->preload_ahead == 2);
    CHECK(legato::find_preset("missing") == nullptr);
    CHECK(std::strcmp(legato::default_preset()->name, "vod") == 0);
}

void test_errors() {
    struct Expect {
        const char* name;
        const char* category;
        bool retryable;
    };
    const Expect rows[] = {
        {"ok", "internal", false},
        {"invalid_argument", "policy", false},
        {"unsupported", "policy", false},
        {"source_open_failed", "source", false},
        {"unsupported_scheme", "source", false},
        {"demux_failed", "demux", false},
        {"seek_failed", "demux", false},
        {"decode_failed", "decode", false},
        {"audio_device_unavailable", "audio_device", false},
    };
    for (const Expect& row : rows) {
        const legato::ErrorInfo* info = legato::error_info(row.name);
        CHECK(info != nullptr);
        if (info == nullptr) {
            continue;
        }
        CHECK(std::strcmp(info->category, row.category) == 0);
        CHECK(info->retryable == row.retryable);
    }
    CHECK(legato::error_info("missing") == nullptr);
    char path[64];
    CHECK(legato::local_file_path("file:///tmp/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/a.mp4") == 0);
    CHECK(legato::local_file_path("file://localhost/tmp/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/a.mp4") == 0);
    CHECK(legato::local_file_path("file:///C:/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "C:/a.mp4") == 0);
    CHECK(legato::local_file_path("file://server/share/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "//server/share/a.mp4") == 0);
    CHECK(legato::local_file_path("corpus://startup/local-1080p-h264", path, sizeof(path)) == 0);
    CHECK(legato::local_file_path("file:///tmp/a%20b.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/a b.mp4") == 0);
    CHECK(legato::local_file_path("file:///C:/a%20b.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "C:/a b.mp4") == 0);
    CHECK(legato::local_file_path("file://server/share/a%20b.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "//server/share/a b.mp4") == 0);
    CHECK(legato::local_file_path("file:///%43%3A/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "C:/a.mp4") == 0);
    CHECK(legato::local_file_path("file:///tmp/100%25x.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/100%x.mp4") == 0);
    CHECK(legato::local_file_path("file:///tmp/a%zz.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/a%zz.mp4") == 0);
    CHECK(legato::local_file_path("file:///tmp/a%00b.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/a%00b.mp4") == 0);
    CHECK(legato::local_file_path("/tmp/a%20b.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "/tmp/a%20b.mp4") == 0);
}

void test_json_fixed() {
    char buf[32];
    CHECK(legato::json_fixed6(0.0, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "0.000000") == 0);
    CHECK(legato::json_fixed6(0.5, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "0.500000") == 0);
    CHECK(legato::json_fixed6(1.25, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "1.250000") == 0);
    CHECK(legato::json_fixed6(12.0, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "12.000000") == 0);
    CHECK(legato::json_fixed6(-0.5, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "-0.500000") == 0);
    CHECK(!legato::json_fixed6(0.5, buf, 4));
    const char* names[] = {"en_DK.utf8", "en_DK"};
    bool comma_locale = false;
    for (const char* name : names) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            comma_locale = true;
            break;
        }
    }
    if (comma_locale) {
        char printf_buf[32];
        std::snprintf(printf_buf, sizeof(printf_buf), "%.6f", 0.5);
        CHECK(std::strcmp(printf_buf, "0,500000") == 0);
        CHECK(legato::json_fixed6(0.5, buf, sizeof(buf)));
        CHECK(std::strcmp(buf, "0.500000") == 0);
        CHECK(legato::json_fixed6(0.0, buf, sizeof(buf)));
        CHECK(std::strcmp(buf, "0.000000") == 0);
        std::setlocale(LC_NUMERIC, "C");
    }
}

void test_win_path() {
    char path[512];
    CHECK(legato::win32_extended_path("C:/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "\\\\?\\C:\\a.mp4") == 0);
    CHECK(legato::win32_extended_path("C:\\a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "\\\\?\\C:\\a.mp4") == 0);
    CHECK(legato::win32_extended_path("//server/share/a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "\\\\?\\UNC\\server\\share\\a.mp4") == 0);
    CHECK(legato::win32_extended_path("\\\\server\\share\\a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "\\\\?\\UNC\\server\\share\\a.mp4") == 0);
    CHECK(legato::win32_extended_path("foo/bar.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "foo\\bar.mp4") == 0);
    CHECK(legato::win32_extended_path("C:foo.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "C:foo.mp4") == 0);
    CHECK(legato::win32_extended_path("\\\\?\\C:\\a.mp4", path, sizeof(path)) == 1);
    CHECK(std::strcmp(path, "\\\\?\\C:\\a.mp4") == 0);
    CHECK(legato::win32_extended_path("", path, sizeof(path)) == 0);
    CHECK(legato::win32_extended_path("//", path, sizeof(path)) == 0);
    char tiny[8];
    CHECK(legato::win32_extended_path("C:/abcdef.mp4", tiny, sizeof(tiny)) == -1);
    std::string long_path = "C:/";
    long_path.append(300, 'a');
    long_path += ".mp4";
    CHECK(legato::win32_extended_path(long_path.c_str(), path, sizeof(path)) == 1);
    CHECK(std::strncmp(path, "\\\\?\\C:\\", 6) == 0);
    CHECK(std::strchr(path, '/') == nullptr);
}

}  /* namespace */

int main() {
    test_spsc();
    test_pool();
    test_actor();
    test_alloc();
    test_clock_and_presets();
    test_errors();
    test_json_fixed();
    test_win_path();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
