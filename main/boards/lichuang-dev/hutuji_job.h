#ifndef HUTUJI_JOB_H
#define HUTUJI_JOB_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace hutuji {

/**
 * @brief M2 出图任务：下载 → CRC → 授权门 → Telnet 逐行转发。
 * MCP 回调只调 StartDraw / RequestAbort / StatusJson，不阻塞。
 */
class Job {
public:
    static Job& GetInstance();

    /** 启动独立任务。busy 时返回 "busy"。 */
    std::string StartDraw(const std::string& url);

    /** 分状态 abort（protocol §4.1）。 */
    std::string RequestAbort();

    /** 暂停当前出图：发 `!` 进给保持，转发循环停在行边界。 */
    std::string RequestPause();

    /** 恢复暂停的出图：发 `~` 继续。 */
    std::string RequestResume();

    /**
     * 重画上一张：复用 PSRAM 里留存的 G-code，跳过下载与 CRC。
     * 无留存内容时回落到用上次 url_ 重新下载。
     */
    std::string RequestRepeat();

    /** 笔测试：M3 落笔 → 停 1s → M5 抬笔，确认笔能触纸。 */
    std::string RequestPenTest();

    /** status JSON：connected/ready/authorized/state/last_line */
    std::string StatusJson() const;

    bool IsPaperActive() const { return paper_active_.load(); }

private:
    Job() = default;

    static void TaskEntry(void* arg);
    void Run();

    bool DownloadToPsram(const std::string& url);
    bool VerifyCrc();
    bool StreamToGrbl();
    void ReleaseBuffer();
    void SetState(const char* state);
    /** 按 paused_ 真值写 streaming/paused，避免状态谎报。 */
    void SetStreamingOrPaused();
    void Notify(const std::string& message);

    static uint32_t Crc32Ieee(const uint8_t* data, size_t len);
    static bool LooksLikePaperLine(const std::string& line);
    static bool LooksLikeMotionLine(const std::string& line);

    void UpdateDisplayProgress();
    size_t CountLines() const;

    /**
     * 暂停期间阻塞等待，直到恢复、abort、链路丢失或超过 kMaxPauseMs。
     * @return true 可继续转发；false 应终止（原因已写入 last_error_）
     */
    bool WaitWhilePaused();

    std::string url_;
    uint8_t* buffer_ = nullptr;
    size_t buffer_len_ = 0;
    uint32_t expect_crc_ = 0;
    bool have_crc_ = false;

    mutable std::mutex state_mutex_;
    // 协调 pause 与 SendLine：暂停字符发出后禁止再灌入新行。
    std::mutex stream_mutex_;
    std::string state_{"idle"};
    std::string last_error_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> paper_active_{false};
    std::atomic<bool> paused_{false};
    // 重画：跳过下载/校验，直接复用 buffer_
    std::atomic<bool> repeat_mode_{false};
    // buffer_ 是否留存着可重画的 G-code（出图成功后不释放）
    std::atomic<bool> buffer_replayable_{false};
    // 试笔期间不接受暂停/恢复，避免 M3/M5 被实时进给保持打断。
    std::atomic<bool> pen_test_active_{false};

    size_t lines_total_ = 0;
    size_t lines_sent_ = 0;
};

} // namespace hutuji

#endif // HUTUJI_JOB_H
