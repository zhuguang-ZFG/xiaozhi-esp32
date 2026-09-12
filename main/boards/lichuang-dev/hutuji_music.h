#ifndef HUTUJI_MUSIC_H
#define HUTUJI_MUSIC_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "hutuji_music_core.h"

/**
 * 唱歌播放泵：下载 OGG 到 PSRAM，经 OggDemuxer 增量喂给 AudioService 的
 * 解码队列（与 PlaySound 同链路），不改 audio_service 任何行为。
 *
 * 线程模型：一个短命工作任务完成「下载→投喂」全程；停止标志在解封装回调
 * 与下载读循环里检查；设备离开 idle/speaking（用户喊小派、按键说话）时
 * 自动停，保证音乐绝不抢对话。
 */
namespace hutuji {

class HutujiMusic {
public:
    static HutujiMusic& GetInstance();

    /** 校验 URL 并起播；已在播则先停旧歌。返回 false 时 last_error 可读。 */
    bool Play(const std::string& url, const std::string& title);
    /** 请求停止并清解码队列（即刻静音）；幂等。 */
    void Stop();
    /** 等待工作任务完全退出（下载连接已关、PSRAM 已还）。 */
    bool WaitStopped(uint32_t timeout_ms);
    bool IsActive() const;
    std::string CurrentTitle() const;
    std::string LastError() const;

private:
    HutujiMusic() = default;
    ~HutujiMusic() = default;
    HutujiMusic(const HutujiMusic&) = delete;
    HutujiMusic& operator=(const HutujiMusic&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool DownloadToPsram(const std::string& url);
    void PumpDecoded();
    void TransitionTo(music::MusicState next);
    void SetError(const std::string& error);
    /** 用户介入（listening 等）时不应继续放歌。 */
    static bool DeviceStateAllowsMusic();

    mutable std::mutex mutex_;
    music::MusicState state_ = music::MusicState::kIdle;
    std::atomic<bool> stop_requested_{false};
    std::string url_;
    std::string title_;
    std::string last_error_;
    uint8_t* buffer_ = nullptr;
    size_t buffer_len_ = 0;
    uint32_t expect_crc_ = 0;
};

}  // namespace hutuji

#endif  // HUTUJI_MUSIC_H
