#ifndef LEGATO_ENGINE_FILE_PLAYBACK_HPP
#define LEGATO_ENGINE_FILE_PLAYBACK_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace legato {

class Player;

/* 本地文件会话：DataSource → FFmpeg 解复用 → 软解 → ALSA。
   工作线程一直活到 stop_and_join。命令只改标志，不在持有 Player 锁时 join，
   否则工作线程回调 Player 时会和自己死锁。 */

class FilePlayback {
public:
    explicit FilePlayback(Player* player);
    ~FilePlayback();

    FilePlayback(const FilePlayback&) = delete;
    FilePlayback& operator=(const FilePlayback&) = delete;

    int open(const char* path);
    void start();
    void pause();
    void request_seek(std::int64_t position_us);
    void stop_and_join();

    std::int64_t duration_us() const { return duration_us_; }

private:
    struct Impl;
    void run();

    Player* player_ = nullptr;
    std::unique_ptr<Impl> impl_;
    std::int64_t duration_us_ = 0;
    std::mutex wait_mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    bool at_end_ = false;
    std::atomic<bool> seek_req_{false};
    std::int64_t seek_to_ = 0;
};

}  /* namespace legato */

#endif
