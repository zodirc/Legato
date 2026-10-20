#include "legato/render/sink.hpp"

#include <atomic>

namespace legato {
namespace {

std::atomic<std::uint64_t> g_audio_frames{0};
std::atomic<std::uint64_t> g_video_frames{0};

}  /* namespace */

void null_audio_present(std::int64_t /*pts_us*/) {
    g_audio_frames.fetch_add(1, std::memory_order_relaxed);
}

void null_video_present(std::int64_t /*pts_us*/) {
    g_video_frames.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t null_audio_frames() { return g_audio_frames.load(std::memory_order_relaxed); }
std::uint64_t null_video_frames() { return g_video_frames.load(std::memory_order_relaxed); }

}  /* namespace legato */
