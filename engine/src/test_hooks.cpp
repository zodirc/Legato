#include "legato/engine/test_hooks.hpp"

#include <atomic>
#include <cstdlib>

namespace legato {
namespace {

std::atomic<std::int64_t> g_open_delay_us{0};
std::atomic<int> g_last_seek_mode{-1};
std::atomic<int> g_video_decoder_opens{0};
std::atomic<int> g_video_packets{0};
std::atomic<int> g_pcm_output{0};
std::atomic<std::int64_t> g_decode_delay_us{0};
std::atomic<int> g_decode_delay_started{0};
std::atomic<int> g_decode_delay_applied{0};
std::atomic<int> g_pcm_pops{0};
std::atomic<int> g_pcm_xruns{0};
std::atomic<int> g_hold_demux{0};

int drop_from_env() {
    const char* text = std::getenv("LEGATO_TEST_DROP_VIDEO_FRAMES");
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    return std::atoi(text);
}

}  /* namespace */

void set_test_open_delay_us(std::int64_t delay_us) {
    g_open_delay_us.store(delay_us < 0 ? 0 : delay_us, std::memory_order_release);
}

std::int64_t test_open_delay_us() { return g_open_delay_us.load(std::memory_order_acquire); }

int test_drop_video_frames() { return drop_from_env(); }

void note_test_seek_mode(int mode) { g_last_seek_mode.store(mode, std::memory_order_release); }

int test_last_seek_mode() { return g_last_seek_mode.load(std::memory_order_acquire); }

void note_test_video_decoder() { g_video_decoder_opens.fetch_add(1, std::memory_order_relaxed); }

int test_video_decoder_opens() { return g_video_decoder_opens.load(std::memory_order_acquire); }

void reset_test_video_decoder_opens() { g_video_decoder_opens.store(0, std::memory_order_release); }

void note_test_video_packet() { g_video_packets.fetch_add(1, std::memory_order_relaxed); }

int test_video_packets() { return g_video_packets.load(std::memory_order_acquire); }

void reset_test_video_packets() { g_video_packets.store(0, std::memory_order_release); }

void set_test_pcm_output(int enable) {
    g_pcm_pops.store(0, std::memory_order_release);
    g_pcm_xruns.store(0, std::memory_order_release);
    g_pcm_output.store(enable ? 1 : 0, std::memory_order_release);
}

int test_pcm_output() { return g_pcm_output.load(std::memory_order_acquire); }

void kick_test_decode_delay_us(std::int64_t delay_us) {
    g_decode_delay_started.store(0, std::memory_order_release);
    g_decode_delay_applied.store(0, std::memory_order_release);
    g_decode_delay_us.store(delay_us > 0 ? delay_us : 0, std::memory_order_release);
}

std::int64_t take_test_decode_delay_us() { return g_decode_delay_us.exchange(0, std::memory_order_acq_rel); }

void note_test_decode_delay_started() { g_decode_delay_started.store(1, std::memory_order_release); }

void note_test_decode_delay_applied() { g_decode_delay_applied.store(1, std::memory_order_release); }

int test_decode_delay_started() { return g_decode_delay_started.load(std::memory_order_acquire); }

int test_decode_delay_applied() { return g_decode_delay_applied.load(std::memory_order_acquire); }

void note_pcm_pop() { g_pcm_pops.fetch_add(1, std::memory_order_relaxed); }

void note_pcm_xrun() { g_pcm_xruns.fetch_add(1, std::memory_order_relaxed); }

int test_pcm_pops() { return g_pcm_pops.load(std::memory_order_acquire); }

int test_pcm_xruns() { return g_pcm_xruns.load(std::memory_order_acquire); }

void set_test_hold_demux(int hold) { g_hold_demux.store(hold ? 1 : 0, std::memory_order_release); }

int test_hold_demux() { return g_hold_demux.load(std::memory_order_acquire); }

}  /* namespace legato */
