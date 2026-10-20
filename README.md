# Legato 播放链路和字段详解

Legato 是跨平台音视频播放引擎。实现语言为 C++20，稳定接口为 C ABI，符号前缀 `legato_`。解复用与解码委托 FFmpeg；数据源、会话状态机、主时钟和音频输出由引擎实现。上层通过属性、命令和事件驱动会话。

本仓库包含三个交付件：

- `liblegato.so`：引擎动态库。版本脚本 `third_party/ffmpeg/liblegato.version` 只导出 `legato_`。
- `legato_play`：本地文件播放示例。PCM 经 ALSA 输出，直至流结束（EOS）。
- `legato-bench`：确定性回归工具。对 `corpus://` 用例推进虚拟媒体时钟，将首帧时延、事件序和稳态分配与基线比对。

视频基本流会被解码。当前未接入显示 Sink，解码帧在 `receive` 返回后释放，不呈现到窗口。

本文档按统一三段式编排：

1. 代码流程（入口、URI 分发、本地播放、确定性回归、传输控制）
2. 字段对应（命令、属性、状态、错误、播放位置的来源）
3. 构建、目录与验证

> 最后更新时间：2026-10-20
> 基于路径：`Legato`

---

## 1. 代码流程（调用时机 + 数据流转）

### 1.1 命令入口到会话
1. **进程入口**
   - `examples/play.c` 创建 `legato_engine` 与 `legato_player`，预设为 `music`。
   - `legato_player_load` 将 `legato_media_item.uri` 提交至 `Player::load_uri`。
   - 命令在控制线程上串行执行，调用方阻塞至该轮结束。返回值 0 表示命令已受理，不表示媒体已打开。

2. **URI 分发（`Player::load_locked`）**
   - `corpus:`：进入确定性回归路径，见 1.3。该 scheme 是用例标识，不映射到文件系统。
   - 含 `://` 且 scheme 不是 `file`：`state=error`，`message=这个协议还不能播`。
   - `file://`、`file://localhost` 以及无 scheme 的路径：进入 `FilePlayback`。路径不存在时仍走打开流程，由 `open` 返回错误。
   - 路径长度达到或超过 4096 字节：`message=路径太长`。空 URI：`message=uri 为空`。

3. **会话状态**
   - 打开成功：`state=loading`。若 `playing_intent_` 已置位，`FilePlayback::start` 立即启动解码线程。
   - 打开失败：释放 `playback_`，`file_mode_=false`，`state=error`。

---

### 1.2 本地文件播放（FilePlayback）

管线为 `FileSource` → `FFmpegDemuxer` → `FFmpegSwDecoder` → `AlsaSink`。

1. **打开**
   - `FileSource::open_file` 使用 `fopen` / `fread` / `fseeko`。
   - `FFmpegDemuxer::open` 分配 32KB AVIO 缓冲区，读与 seek 回调进入 `DataSource`。`AVFormatContext` 设置 `AVFMT_FLAG_CUSTOM_IO`，不使用 FFmpeg 内置文件协议。
   - 存在音频流时必须打开 `AlsaSink`。设备尝试顺序为 `default`、`plughw:0,0`、`hw:0,0`。输出格式为交错 S16，采样率取 `snd_pcm_hw_params_set_rate_near` 的协商结果。
   - 视频解码器打开失败时：会话仍有音频流则忽略该路；会话只有视频流则整次打开失败。
   - 容器与编码范围由 `third_party/ffmpeg/configs/play-demo.config` 决定。容器：mov、matroska、flv、mpegts、aac、mp3、ogg、wav。音频：aac、mp3、flac、pcm s16。视频：h264。

2. **解码线程**
   - `start` 将 `playing_` 置位。线程在会话生命周期内只创建一次，阻塞在条件变量上，直到 `playing_` 或 `stop_`。
   - 音频包路径：`decode` → `receive` → `snd_pcm_writei`。首次写入成功时调用 `Player::media_first_frame`，`state` 转为 `playing`。
   - 视频包路径：解码后释放帧缓冲。无音频设备时，`pace_video` 以首帧对齐单调时钟，后续帧按 PTS 间隔休眠。
   - 读包、解码或 PCM 写入失败：调用 `Player::media_failed`，清除 `playing_`，线程回到等待。线程保持存活，以便后续 `play` 仍能调度。

3. **流结束**
   - `read_packet` 返回 EOF 后向解码器发送空包，刷新内部缓冲。
   - 音频路径再调用 `FFmpegSwDecoder::pull_delayed`，取出 `SwrContext` 中的残余样本。
   - `snd_pcm_drain` 播放完设备缓冲。若此时 `playing_` 已清除或存在待处理 seek，不发送 EOS。
   - 否则将 `position_us` 置为时长，`state=ended`，事件载荷为 `{"reason":"eos"}`。
   - 对已结束的会话再次 `play`：解码线程 seek 至 0 后重新输出。

4. **主时钟**
   - 音频主时钟：`position_us = origin_us + (written_frames - snd_pcm_delay) * 1000000 / rate`。`snd_pcm_delay` 为设备中尚未播放的帧数。
   - 无音频流时：`position_us` 取视频 PTS。

---

### 1.3 确定性回归（`corpus://`）

确定性回归用于在不依赖媒体文件和音频设备的前提下，复现会话的首帧时延、事件序和稳态内存分配。它不是第二种播放器，也不输出 PCM。

1. **入口**
   - `examples/hello.c`、`tests/smoke.c`、`legato-bench run bench/scenarios/<name>.yaml`。
   - URI scheme 必须为 `corpus`。示例：`corpus://startup/local-1080p-h264`。
2. **虚拟媒体时钟**
   - `load_locked` 将状态置为 `loading`，并按预设登记首帧定时器。
   - 点播（`vod`）首帧时延 120ms；短视频（`short_video`）冷启动 100ms，解码器复用 80ms。
   - 时钟只在 `legato_engine_advance_us` 被调用时前进。定时器到期后发送 `first_frame`，`state` 转为 `playing`。
3. **基线比对**
   - `legato-bench` 将运行结果与 `bench/golden/` 比对。结果字段 `model` 的值为 `headless-skeleton`，表示该基线来自虚拟时钟，不是音频设备实测。
   - `alloc_steady` 统计进入稳态后的 `tracked_malloc` 次数，门禁为 0。
   - 基线中的时延是预设常量。本地文件播放的实测时延不回写这些文件。
4. **与本地播放的隔离**
   - 该路径不创建 `FileSource`、解复用器、解码器和 `AlsaSink`。
   - 本地播放的音频时钟不推进 `Scheduler`。两条路径的时间基相互独立。

---

### 1.4 暂停、Seek 和停止
1. **暂停**
   - 控制线程清除 `playing_`，并调用 `AlsaSink::drop`。
   - `drop` 先置 `interrupted_`，再调用 `snd_pcm_drop`，使阻塞在 `snd_pcm_writei` 中的解码线程返回。
   - 设备欠载（XRUN）仍调用 `snd_pcm_recover`。`interrupted_` 置位时跳过 recover，以免把已暂停的 PCM 设备重新置为运行态。
2. **Seek**
   - `legato_player_seek` 记录目标时间。解码线程执行 `av_seek_frame`，再 `flush` 解码器。
   - `flush` 调用 `swr_init`，丢弃 seek 点之前残留在重采样器中的样本。
   - `av_seek_frame` 失败：`message=定位失败`，`state=error`。
3. **停止**
   - `stop_and_join` 置 `stop_`，对 PCM 设备执行 `drop`，然后 join 解码线程。
   - 析构和切换媒体项都走该函数，且发生在重新获取 `Player::mu_` 之前。解码线程回调 `media_first_frame` 时需要同一把锁；持锁 join 会形成死锁。

---

## 2. 字段对应

格式：`对外字段 <- 引擎字段 <- 产生位置`。运行时生成的值写明函数。

### 2.0 命令与属性
1. **命令**
   - `legato_player_load` <- `Player::load_uri` <- `examples/play.c`
   - `legato_player_play` / `pause` / `seek` <- `play_locked` / `pause_locked` / `seek_locked` <- 控制线程
   - `legato_player_get` <- `Player::get_json` <- 属性名定义于 `include/legato/legato_idl.h`
   - `legato_player_position_us` <- `position_us_` <- 音频主时钟或视频 PTS
2. **属性**
   - `state` <- `Player::state_` <- `legato_state_name`。JSON 为带引号字符串，例如 `"playing"`
   - `position_us` <- `position_us_` <- `set_media_position`，或确定性回归中的 `Scheduler`
   - `duration_us` <- `duration_us_` <- `FFmpegDemuxer::duration_us`（`AVFormatContext.duration`，单位微秒）
   - `preset` <- `preset_->name` <- 创建参数。`legato_play` 使用 `music`
   - `last_error` <- `last_error_` <- `Player::set_error_locked`
   - `playing_intent` <- `playing_intent_` <- `play` / `pause`
   - `serial` <- `serial_` <- 每次 seek 递增
   - `video.output_path` <- `output_path_` <- 诊断属性。当前未接入显示 Sink

### 2.1 `state`
- `idle` <- `stop_locked`
- `loading` <- `load_file_locked`，或确定性回归的 `load_locked`
- `ready` <- 本地会话暂停；或确定性回归首帧到达时 `playing_intent_` 为假
- `playing` <- `media_first_frame` 且 `playing_intent_` 为真
- `ended` <- `media_ended`，事件载荷 `{"reason":"eos"}`
- `error` <- `set_error_locked`
- `buffering` 存在于状态枚举。本地播放和确定性回归均不进入该状态

### 2.2 `last_error`
- `category` <- `error_info(code)->category`
- `code` <- 调用点传入的错误名。打开失败为 `invalid_argument`，scheme 不支持为 `unsupported`
- `retryable` <- 常量 `false`
- `message` <- 调用点字符串：`打不开这个文件`、`这个协议还不能播`、`路径太长`、`uri 为空`、`读包失败`、`音频解码失败`、`视频解码失败`、`声卡写入失败`、`定位失败`
- `hint` <- 错误表
- `trace_id` <- 常量 `m0`
- `state` 为 `"error"` 时，`legato_play` 将整段 JSON 写入标准错误，进程退出码为 1

### 2.3 播放位置与首帧时延
- `position_us` <- `origin_us + (written_frames - delay_frames) * 1000000 / rate` <- `FilePlayback::note_audio`
- `position_us` <- 视频 PTS <- `pace_video`（仅无音频设备）
- `position_us` <- `duration_us` <- `media_ended`
- `first_frame.us` <- `monotonic_us() - file_load_mono_` <- 首次成功写入 ALSA，或纯视频流的首个解码帧
- 确定性回归的 `first_frame.us` <- 定时器触发时刻减去 `load_us_` <- `on_first_frame`。数值为 1.3 节所列预设时延

### 2.4 URI
- `uri` <- `legato_media_item.uri` <- `legato_player_load`
- `local_path` 去掉 `file://` 前缀及可选的 `localhost`，剩余字符串交给 `FileSource`
- `corpus://...` 保持原串，作为确定性回归的用例标识，不转换为文件系统路径

---

## 3. 构建、目录与验证

### 3.1 构建条件
1. **编译器**
   - GCC 低于 10 时，`make` 改用 Clang。
   - `make` 只编译。`make test` 编译并执行测试。`make bench` 对照 `bench/golden/`。
   - IDE 预设名为「Clang Debug」和「开发 Debug」。
2. **FFmpeg**
   - 存在 `third_party/ffmpeg/dist/lib/libavformat.a` 时，CMake 定义 `LEGATO_HAS_FFMPEG` 并构建 `legato_play`。
   - 源码树与 `dist` 不纳入版本库。版本 5.1.6，配置名 `play-demo`。
3. **运行时**
   - 本地播放链接 `/usr/lib64/libasound.so.2`，并需要可用的 PCM 播放设备。
   - 未提供该 FFmpeg 构建时，本地文件打开失败。确定性回归用例仍可编译运行。

```bash
mkdir -p third_party/ffmpeg/src
curl -L -o third_party/ffmpeg/src/ffmpeg-5.1.6.tar.xz https://ffmpeg.org/releases/ffmpeg-5.1.6.tar.xz
tar -C third_party/ffmpeg/src -xf third_party/ffmpeg/src/ffmpeg-5.1.6.tar.xz
./third_party/ffmpeg/build-ffmpeg.sh \
  --src third_party/ffmpeg/src/ffmpeg-5.1.6 \
  --config play-demo \
  --prefix third_party/ffmpeg/dist
make test
python3 testdata/play_tone.py ./build/legato_play
./build/legato_play /path/to/clip.wav
```

`testdata/play_tone.py` 生成 `testdata/out/tone.wav`：时长 1.2 秒，48 kHz，立体声，440 Hz。进程应打印「结束」，单调时钟耗时不少于 0.8 秒。`testdata/out/` 不纳入版本库。

修改 `idl/` 后重新生成，并用 `--check` 确认生成物与声明一致：

```bash
python3 tools/idlgen/idlgen.py
python3 tools/idlgen/idlgen.py --check
```

### 3.2 目录
- `idl/`：属性、命令、事件、错误和预设的声明源。`tools/idlgen/idlgen.py` 生成 `include/legato/legato_idl.h`。
- `include/legato/`：C ABI 头文件。
- `core/`：控制线程、单生产者单消费者环、对象池、`tracked_malloc`、日志。
- `pal/`：单调时钟。
- `io/`、`demux/`、`codec/`、`render/`：`FileSource`、`FFmpegDemuxer`、`FFmpegSwDecoder`、`AlsaSink`。
- `engine/`：`Player`、`Scheduler`、预设、`FilePlayback`。
- `api/`：C ABI 实现。
- `bench/`：回归场景与基线。
- `examples/play.c`：本地文件播放。`examples/hello.c`：确定性回归示例。
- `docs/wiki/`：流程架构导览。在线地址是 https://zodirc.github.io/Legato/wiki/ 。Pages 源选择 GitHub Actions，由 `.github/workflows/docs-pages.yml` 发布 `docs/`。
- `testdata/corpus.yaml`：确定性回归用例清单。

### 3.3 示例进程的超时
- `legato_play` 每 100ms 打印 `position_us / 1000` 和带引号的 `state`。
- 已知 `duration_us` 时，超时为媒体时长加 10 秒，且下限为 30 秒。时长未知时超时 30 秒。
- EOS 时退出码为 0。进入 `error` 时退出码为 1。

### 3.4 验证
1. `make test`。`play_tone` 应在约 1.2 秒后打印「结束」。`smoke` 与 `symbols` 通过，动态库导出符号限于 `legato_`。
2. `./build/legato_play /tmp/does-not-exist.wav` 立即以退出码 1 结束，`last_error.message` 为「打不开这个文件」。
3. `./build/legato_hello` 在 `legato_engine_advance_us(120000)` 之后，`position_us` 等于确定性回归的预设值。
4. wav、mp3、aac，以及含 h264 与 aac 的 mp4，经 `legato_play` 应完成 PCM 输出并打印「结束」。视频帧不呈现。

### 3.5 代码依据
- URI 分发与错误：`local_path`、`load_locked`、`load_file_locked`、`set_error_locked` <- `engine/src/player.cpp`
- 打开、读包、EOS、seek、纯视频调度：`FilePlayback::open`、`run` <- `engine/src/file_playback.cpp`
- AVIO 与时间基：`FFmpegDemuxer::open`、`read_packet`、`seek_us` <- `demux/src/ffmpeg_demuxer.cpp`
- 重采样残余与 seek 后清空：`pull_delayed`、`flush` <- `codec/src/ffmpeg_sw_decoder.cpp`
- PCM 设备：`write_frames`、`drop`、`end_interrupt`、`drain`、`delay_frames` <- `render/src/alsa_sink.cpp`
- 示例进程：`examples/play.c`
- 格式开关：`third_party/ffmpeg/configs/play-demo.config`
- 回归基线：`bench/golden/`
