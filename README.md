# Legato 播放链路和字段详解

Legato 是跨平台音视频播放引擎。实现语言为 C++20，稳定接口为 C ABI，符号前缀 `legato_`。解复用与解码委托 FFmpeg；数据源、会话状态机、主时钟和音频输出由引擎实现。上层通过属性、命令和事件驱动会话。

本仓库包含三个交付件：

- `liblegato.so`：引擎动态库。版本脚本 `third_party/ffmpeg/liblegato.version` 只导出 `legato_`。
- `legato_play`：本地文件播放示例。PCM 经 ALSA 输出，直至流结束（EOS）。
- `legato-bench`：确定性回归工具。对 `corpus://` 用例推进虚拟媒体时钟，将首帧时延、事件序和稳态分配与基线比对。

本地文件的视频基本流会被解码。当前未接入显示 Sink：帧交给 `CountingVideoSink`，只计数，不画到窗口。确定性回归把呈现时间戳和帧指纹写入记录 Sink。预设 `video` 为假时不打开视频解码器，也不向解码器送视频包。时钟和 Sink 由 `Player::io_main` 注入。

本文档按统一三段式编排：

1. 代码流程（入口、URI 分发、本地播放、确定性回归、传输控制）
2. 字段对应（命令、属性、状态、错误、播放位置的来源）
3. 构建、目录与验证

> 最后更新时间：2026-10-22
> 基于路径：`Legato`

---

## 1. 代码流程（调用时机 + 数据流转）

### 1.1 命令入口到会话
1. **进程入口**
   - `examples/play.c` 创建 `legato_engine` 与 `legato_player`，预设为 `music`。
   - `legato_player_load` 将 `legato_media_item.uri` 提交至 `Player::load_uri`。
   - `Engine::post` 把命令交给控制线程后立即返回。返回值 0 表示命令已受理，不表示媒体已打开。命令真正结束时发送 `command_done`，载荷为 `{"id":N,"ok":true|false}`。
   - 打开文件发生在会话自己的 IO 线程，不持有 `Engine::mu_` 或 `Player::mu_`。锁顺序仍是先引擎后会话。
   - `legato_player_release` 只标记已释放并投递回收任务。join IO 线程和解码线程发生在回收线程，不发生在调用方。

2. **URI 分发（`Player::begin_load_locked`）**
   - `corpus:`：按 `testdata/corpus.yaml` 核对内容哈希后进入 `FilePlayback`，时钟为虚拟媒体时钟。见 1.3。
   - 含 `://` 且 scheme 不是 `file` 或 `corpus`：`state=error`，错误名 `unsupported_scheme`，`message=这个协议还不能播`。
   - `file://`、`file://localhost` 以及无 scheme 的路径：进入 `FilePlayback`。有音频流时时钟为 `AudioMasterClock`，没有音频流时改为 `MonotonicClock`。路径不存在时仍走打开流程，由 `open` 返回错误。
   - 路径长度达到或超过 4096 字节：错误名 `invalid_argument`，`message=路径太长`。空 URI：`message=uri 为空`。

3. **会话状态**
   - 打开开始：`state=loading`。打开成功且 `playing_intent_` 已置位时，`FilePlayback::start` 启动解复用线程、每路解码线程和音频输出线程。
   - 打开失败：不安装管线，`state=error`，并发送 `command_done` 且 `ok` 为假。

---

### 1.2 本地文件播放（FilePlayback）

管线为 `FileSource` → `FFmpegDemuxer` → 每路包队列 → `FFmpegSwDecoder` → PCM 环或帧队列 → 音频输出线程。`Player::io_main` 注入 `Clock`、`AudioSink` 和 `VideoSink`。虚拟媒体时钟上音频是 `NullAudioSink`，视频是 `RecordingVideoSink`。本地文件的视频是 `CountingVideoSink`。

1. **打开**
   - `FileSource::open_file` 使用 `fopen` / `fread` / `fseeko`。
   - `FFmpegDemuxer::open` 分配 32KB AVIO 缓冲区，读与 seek 回调进入 `DataSource`。`AVFormatContext` 设置 `AVFMT_FLAG_CUSTOM_IO`，不使用 FFmpeg 内置文件协议。
   - 存在音频流时必须打开 `AlsaSink`。设备尝试顺序为 `default`、`plughw:0,0`、`hw:0,0`。输出格式为交错 S16，采样率取 `snd_pcm_hw_params_set_rate_near` 的协商结果。
   - 预设 `video` 为假时不创建视频解码器。视频解码器打开失败时：会话仍有音频流则忽略该路；会话只有视频流则整次打开失败。
   - 容器与编码范围由 `third_party/ffmpeg/configs/play-demo.config` 决定。容器：mov、matroska、flv、mpegts、aac、mp3、ogg、wav。音频：aac、mp3、flac、pcm s16。视频：h264。

2. **线程**
   - 解复用线程读包，按流放进各自的包队列。队列用预设的 `buffer_max_s` 做时长上限，字节上限是这个秒数乘以 1 MiB/s。空队列仍接收一个包。
   - 音频解码线程和视频解码线程各自从自己的包队列取包。视频帧队列容量 8。PCM 放进环，音频输出线程从环里取出再 `snd_pcm_writei`。没有音频流时，`VideoPresenter` 按 PTS 对齐单调时钟。
   - 包和帧带 `serial`。控制线程在 Seek 时只写 `serial` 和目标位置。队列里旧 `serial` 的数据被丢掉。PCM 输出线程看到新 `serial` 后，把自己的写入帧数和首帧标记清零。
   - 首次写入成功后，输出线程把首帧放进通知环。控制线程取出后调用 `Player::media_first_frame`，`state` 转为 `playing`。
   - 起播后包队列见底，输出线程写入卡顿通知。控制线程把 `state` 转为 `buffering`，`stall_count` 加一，`playing_intent` 仍为真。缓冲回到 `buffer_resume_s` 后回到 `playing`。
   - 读包或解码失败：解复用或解码线程调用 `Player::media_failed`，参数是错误名和文案，并清除 `playing_`。PCM 写入失败由输出线程把 `audio_device_unavailable` 写入通知环。线程保持存活，以便后续 `play` 仍能调度。

3. **流结束**
   - `read_packet` 返回 EOF 后向解码器发送空包，刷新内部缓冲。
   - 音频路径再调用 `FFmpegSwDecoder::pull_delayed`，取出 `SwrContext` 中的残余样本。
   - `snd_pcm_drain` 播放完设备缓冲。若此时 `playing_` 已清除或存在待处理 seek，不发送 EOS。
   - 否则将 `position_us` 置为时长，`state=ended`，事件载荷为 `{"reason":"eos"}`。
   - 对已结束的会话再次 `play`：解码线程 seek 至 0 后重新输出。

4. **主时钟**
   - 音频主时钟只在 `AudioMasterClock::observe_delay` 里计算：`position_us = origin_us + (written_frames - delay_frames) * 1000000 / rate`。`delay_frames` 来自 `AudioSink::delay_frames`，ALSA 上是 `snd_pcm_delay`。
   - 无音频流时：`position_us` 取视频 PTS。时钟是 `MonotonicClock`。

---

### 1.3 确定性回归（`corpus://`）

确定性回归打开 `testdata/corpus.yaml` 指向的媒体，用虚拟媒体时钟驱动呈现，从而复现首帧时延、事件序、呈现序列哈希和稳态内存分配。它不打开 ALSA PCM 设备。

1. **入口**
   - `examples/hello.c`、`tests/smoke.c`、`legato-bench run bench/scenarios/<name>.yaml`。
   - `resolve_corpus` 核对清单中的 SHA-256。清单项带 `path` 时 URI 必须整段相等。带 `path_prefix` 时 URI 形如 `corpus://feed/portrait-1080p-h264/0`。
2. **虚拟媒体时钟**
   - 时钟只在 `legato_engine_advance_us` 被调用时前进。调用线程先等到起播缓冲够了，控制线程再按 16667 微秒的垂直同步量化；每一步都等这一步的 PCM 和视频帧。
   - 第一帧在第一次垂直同步呈现。`first_frame.us` 为 16667，与解码耗时无关。
   - 播放位置为 `media_start +` 自本次播放或 Seek 起的虚拟时间。Seek 发生在 load 之前时，`media_start` 仍是 Seek 目标。
   - `Player::io_main` 注入 `VirtualClock`、`NullAudioSink` 和 `RecordingVideoSink`。`VideoPresenter::present_until` 按 16667 微秒挑帧。`NullAudioSink::write_frames` 消费样本，样本位置经 `AudioMasterClock::observe_delay` 算出。`RecordingVideoSink` 记录呈现 PTS 序列哈希，写入 `presentation_sha256`，并计算 `|视频 PTS - 音频时钟|`。`video.output_path` 在该路径上为 `record`。
3. **基线比对**
   - `legato-bench` 将运行结果与 `bench/golden/` 比对。结果字段 `model` 的值为 `pipeline`。
   - `alloc_steady` 统计进入稳态后的 `tracked_malloc` 次数，门禁为 0。FFmpeg 自己的分配不计入。
   - `av_sync_us` 由 `RecordingVideoSink` 取 `|视频 PTS - 音频时钟|` 的最大偏差。门禁为 p95 小于 40 毫秒。偏差小于 1 毫秒时，毫秒统计可以显示为 0。
4. **与本地播放的差别**
   - 同一条 `FilePlayback` 管线，不按运行形态分叉。确定性回归注入 `VirtualClock` 和记录 Sink。本地文件注入 `AudioMasterClock` 和 ALSA；没有音频流时改为 `MonotonicClock`。

---

### 1.4 暂停、Seek 和停止
1. **暂停**
   - 控制线程清除 `playing_`，并调用 `AlsaSink::drop`。
   - `drop` 先置 `interrupted_`，再调用 `snd_pcm_drop`，使阻塞在 `snd_pcm_writei` 中的音频输出线程返回。
   - 设备欠载（XRUN）仍调用 `snd_pcm_recover`。`interrupted_` 置位时跳过 recover，以免把已暂停的 PCM 设备重新置为运行态。
2. **Seek**
   - `legato_player_seek` 记录目标时间并增加 `serial`。解复用线程执行 `av_seek_frame`。解码线程在自己的线程上 `flush`。
   - `flush` 调用 `swr_init`，丢弃 seek 点之前残留在重采样器中的样本。
   - `av_seek_frame` 失败：错误名 `seek_failed`，`message=定位失败`，`state=error`。错误名由调用点传入，不从文案反推。
3. **停止**
   - `FilePlayback::stop` 置 `stop_`，对 PCM 设备执行 `drop`，然后 join 解复用、解码和音频输出线程。
   - 切换媒体项和回收都在放下 `Player::mu_` 之后调用它。解复用线程调用 `media_failed` 时需要这把锁；持锁 join 会形成死锁。输出线程不拿这把锁。
   - JSON 命令省略 `mode` 时，Seek 模式取预设的 `exact_seek`：为真则 `precise`，否则 `keyframe`。显式传入的模式优先。帧精确的丢帧对齐不在本阶段。

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
   - `position_us` <- `position_us_` <- 音频主时钟，或虚拟媒体时钟上的 `media_start + 已经过的虚拟时间`
   - `duration_us` <- `duration_us_` <- `FFmpegDemuxer::duration_us`（`AVFormatContext.duration`，单位微秒）
   - `preset` <- `preset_->name` <- 创建参数。`legato_play` 使用 `music`
   - `last_error` <- `last_error_` <- `Player::set_error_locked`
   - `playing_intent` <- `playing_intent_` <- `play` / `pause`
   - `serial` <- `serial_` <- 每次 seek 递增
   - `video.output_path` <- `output_path_` <- 创建时为 `none`。确定性回归安装记录 Sink 后为 `record`。`attach_surface` 之后为 `direct`
3. **事件**
   - `error`、`ended`、`first_frame`、`state_changed` 不被环覆盖。Seek 互相合并。同一属性的 `property_changed` 互相合并。放不下时 `events_dropped` 增加。
   - `legato_player_observe` 在控制线程上按 `min_interval_s` 比较属性。值变了才发 `property_changed`。

### 2.1 `state`
- `idle` <- `stop_locked`
- `loading` <- `begin_load_locked`
- `ready` <- 本地会话暂停；或确定性回归首帧到达时 `playing_intent_` 为假
- `playing` <- `media_first_frame` 且 `playing_intent_` 为真
- `ended` <- `media_ended`，事件载荷 `{"reason":"eos"}`
- `error` <- `set_error_locked`
- `buffering` <- `note_stall`。包队列见底且当前是 `playing` 时进入。`playing_intent` 保持为真。缓冲回到 `buffer_resume_s` 后回到 `playing`

### 2.2 `last_error`
- `category` <- `error_info(code)->category`
- `code` <- 调用点传入的错误名。打开或哈希失败为 `source_open_failed`，scheme 不支持为 `unsupported_scheme`，解复用失败为 `demux_failed`，Seek 失败为 `seek_failed`，解码失败为 `decode_failed`，PCM 设备打开失败为 `audio_device_unavailable`。空 URI 和路径过长仍是 `invalid_argument`
- `retryable` <- `error_info(name)->retryable`，由 `idl/errors.yaml` 生成
- `message` <- 调用点字符串：`打不开这个文件`、`这个协议还不能播`、`路径太长`、`uri 为空`、`读包失败`、`音频解码失败`、`视频解码失败`、`声卡写入失败`、`定位失败`
- `hint` <- 错误表。`unsupported_scheme` 的 hint 写明当前支持 `file` 和 `corpus`
- `trace_id` <- 每次 `load` 生成的 `p-` 加 16 位十六进制，不是常量
- `state` 为 `"error"` 时，`legato_play` 将整段 JSON 写入标准错误，进程退出码为 1

### 2.3 播放位置与首帧时延
- `position_us` <- `origin_us + (written_frames - delay_frames) * 1000000 / rate` <- `AudioMasterClock::observe_delay`
- `position_us` <- 视频 PTS <- `VideoPresenter::drain_main`（仅无音频的本地文件）
- `position_us` <- `media_start + 虚拟时间` <- `Player::on_clock_advanced`（确定性回归）
- `position_us` <- `duration_us` <- 到达时长
- `first_frame.us` <- `monotonic_us() - file_load_mono_` <- 首次成功写入 ALSA，或纯视频流的首个解码帧。输出线程写入通知环，控制线程调用 `media_first_frame`
- 确定性回归的 `first_frame.us` <- 第一次垂直同步的 16667 微秒 <- `VideoPresenter::present_until`

### 2.4 URI
- `uri` <- `legato_media_item.uri` <- `legato_player_load`
- `local_file_path` 去掉 `file://` 前缀及可选的 `localhost`。盘符形式 `file:///C:/a.mp4` 得到 `C:/a.mp4`，`file://host/share` 得到 `//host/share`
- `corpus://...` <- `resolve_corpus` <- 核对 `testdata/corpus.yaml` 后得到媒体路径

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
   - 未提供该 FFmpeg 构建时，本地文件和确定性回归都无法打开媒体。生成用例的主机 `ffmpeg` 位于 `third_party/host/`，不链进 `liblegato`。

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
- `testdata/corpus.yaml`：确定性回归用例清单。媒体字节由 `python3 testdata/generate_synthetic.py --corpus` 生成，不纳入版本库。

### 3.3 示例进程的超时
- `legato_play` 每 100ms 打印 `position_us / 1000` 和带引号的 `state`。
- 已知 `duration_us` 时，超时为媒体时长加 10 秒，且下限为 30 秒。时长未知时超时 30 秒。
- EOS 时退出码为 0。进入 `error` 时退出码为 1。

### 3.4 验证
1. `make test`。`play_tone` 应在约 1.2 秒后打印「结束」。`smoke` 与 `symbols` 通过，动态库导出符号限于 `legato_`。`t1_drop_frame` 在丢掉一帧后必须和 `bench/golden/startup-local.json` 对不上。
2. `./build/legato_play /tmp/does-not-exist.wav` 在打开失败后以退出码 1 结束，`last_error.message` 为「打不开这个文件」。
3. `./build/legato_hello` 在 `advance_us(120000)` 再 `advance_us(5000000)` 之后，`position_us` 为 5120000。
4. wav、mp3、aac，以及含 h264 与 aac 的 mp4，经 `legato_play` 应完成 PCM 输出并打印「结束」。视频帧只计数，不画到窗口。

### 3.5 代码依据
- URI 分发与错误：`local_file_path`、`begin_load_locked`、`resolve_corpus`、`set_error_locked` <- `engine/include/legato/engine/uri_path.hpp`、`engine/src/player.cpp`、`engine/src/corpus.cpp`
- 打开、读包、EOS、seek、纯视频调度：`FilePlayback::open`、`run` <- `engine/src/file_playback.cpp`
- AVIO 与时间基：`FFmpegDemuxer::open`、`read_packet`、`seek_us` <- `demux/src/ffmpeg_demuxer.cpp`
- 重采样残余与 seek 后清空：`pull_delayed`、`flush` <- `codec/src/ffmpeg_sw_decoder.cpp`
- PCM 设备：`write_frames`、`drop`、`end_interrupt`、`drain`、`delay_frames` <- `render/src/alsa_sink.cpp`
- 示例进程：`examples/play.c`
- 格式开关：`third_party/ffmpeg/configs/play-demo.config`
- 回归基线：`bench/golden/`
