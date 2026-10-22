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

}  /* namespace legato */

#endif
