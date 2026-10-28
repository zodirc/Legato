#ifndef LEGATO_ENGINE_TEST_HOOKS_HPP
#define LEGATO_ENGINE_TEST_HOOKS_HPP

#include <cstdint>

namespace legato {

void set_test_open_delay_us(std::int64_t delay_us);
std::int64_t test_open_delay_us();
int test_drop_video_frames();
void note_test_seek_mode(int mode);
int test_last_seek_mode();
void note_test_video_decoder();
int test_video_decoder_opens();
void reset_test_video_decoder_opens();
void note_test_video_packet();
int test_video_packets();
void reset_test_video_packets();

/* 验收用。PCM 输出线程在虚拟会话里也跑，用来看解码延迟会不会造成设备欠载。 */
void set_test_pcm_output(int enable);
int test_pcm_output();
void kick_test_decode_delay_us(std::int64_t delay_us);
std::int64_t take_test_decode_delay_us();
void note_test_decode_delay_started();
void note_test_decode_delay_applied();
int test_decode_delay_started();
int test_decode_delay_applied();
void note_pcm_pop();
void note_pcm_xrun();
int test_pcm_pops();
int test_pcm_xruns();
void set_test_hold_demux(int hold);
int test_hold_demux();
/* 稳态里的下一次 PCM 入队分配一次。用来让 alloc_steady_eq_0 失败。 */
int test_pcm_steady_alloc_once();
std::int64_t test_av_sync_offset_us();
/* 上一帧交给呈现之后，下一次 receive 不应改掉它的像素。 */
void note_retained_frame(const std::uint8_t* still_alive, const std::uint8_t* next);
int test_frame_hold_failed();
int test_frame_hold_checks();
void log_test_quiet();
/* 验收强制两条设备打开都失败，用来覆盖没有 PCM 设备时的回落和拒绝。 */
int test_no_audio_device();

}  /* namespace legato */

#endif
