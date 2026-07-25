#ifndef HUTUJI_JOB_H
#define HUTUJI_JOB_H

#include <atomic>
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
    void Notify(const std::string& message);

    static uint32_t Crc32Ieee(const uint8_t* data, size_t len);
    static bool LooksLikePaperLine(const std::string& line);
    static bool LooksLikeMotionLine(const std::string& line);

    std::string url_;
    uint8_t* buffer_ = nullptr;
    size_t buffer_len_ = 0;
    uint32_t expect_crc_ = 0;
    bool have_crc_ = false;

    mutable std::mutex state_mutex_;
    std::string state_{"idle"};
    std::string last_error_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> paper_active_{false};
};

} // namespace hutuji

#endif // HUTUJI_JOB_H
