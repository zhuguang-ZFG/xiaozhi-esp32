#ifndef HUTUJI_JOB_H
#define HUTUJI_JOB_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

    /** 笔测试：弹簧回位校准 Z0 → Z5 触纸 → Z0 抬笔。 */
    std::string RequestPenTest();

    /** status JSON：connected/ready/authorized/state/last_line */
    std::string StatusJson() const;

    bool IsPaperActive() const { return paper_active_.load(); }

private:
    Job() = default;

    static void TaskEntry(void* arg);
    void Run();

    /**
     * S2：一行在 buffer_ 里的位置（剥注释/首尾空白之后的内容 span）。
     *
     * 存 {offset,len} 而不是 std::string：512KB buffer 上限可含 ~24k 行，
     * 每行一个堆 string 会产生 ~24k 次 30 字节小分配。本板
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048 把 2048 字节以下的分配
     * 强制放内部 RAM，而 ESP32-S3 内部 RAM 仅 ~512KB 且已被 WiFi/LVGL/音频
     * 占去大半 —— 会 OOM。索引表最终约 ~192KB，其大块存储超过阈值后落 PSRAM。
     */
    struct LineSpan {
        uint32_t offset;  // 相对 buffer_ 的字节偏移
        uint32_t len;     // 行长度（不含换行/注释/首尾空白）
    };

    bool DownloadToPsram(const std::string& url);
    bool VerifyCrc();
    bool StreamToGrbl();
    /** 等待弹簧自然回位后，以受控旁路命令把当前抬笔位声明为 Z0。 */
    bool PreparePenOrigin();
    bool WaitForIdle(bool honor_abort, uint32_t timeout_ms);
    /**
     * 等 ok 超时后的兜底判定：Grbl WebUI Telnet 输出无 TX 缓冲，`ok` 与 `?` 状态
     * 报告在同核并发写同一 socket，被抢占的部分写会静默吃掉一个 `ok`（不产生
     * error）。此时机器其实已经把在途行走完。取一份 `?` 之后的新状态报告，若为
     * Idle 且 MPos 已达 spans[from,to) 里最后出现的 X/Y 目标，则认定这批在途行
     * 全部完成。Run/Hold、坐标不符或拿不到新报告一律返回 false（fail closed）。
     */
    bool ConfirmInFlightDoneByStatus(const std::vector<LineSpan>& spans, size_t from, size_t to);
    bool ChangePaperAfterDraw();
    bool RecoverDisconnectedDraw();
    void ReleaseBuffer();
    void SetState(const char* state);
    /** 按 paused_ 真值写 streaming/paused，避免状态谎报。 */
    void SetStreamingOrPaused();
    void Notify(const std::string& message);

    static uint32_t Crc32Ieee(const uint8_t* data, size_t len);
    static bool LooksLikePaperLine(const std::string& line);
    static bool LooksLikeMotionLine(const std::string& line);

    void UpdateDisplayProgress();

    /**
     * 暂停期间阻塞等待，直到恢复、abort、链路丢失或超过 kMaxPauseMs。
     * @return true 可继续转发；false 应终止（原因已写入 last_error_）
     */
    bool WaitWhilePaused();

    /**
     * 把 buffer_ 预解析成行索引，供 StreamToGrbl 预取下一行长度
     * （窗口化流控的 peek 前提）。解析规则与改造前内联逻辑逐字一致。
     */
    std::vector<LineSpan> ParseLines() const;

    /** 取某行的文本视图（零拷贝，指向 buffer_）。 */
    std::string_view LineAt(const LineSpan& s) const {
        return std::string_view(reinterpret_cast<const char*>(buffer_) + s.offset, s.len);
    }

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
    // 试笔期间不接受暂停/恢复；abort 只置标志，不并发 reset 抢占 Z 运动应答。
    std::atomic<bool> pen_test_active_{false};

    bool stream_disconnected_ = false;
    uint32_t stream_connection_seq_ = 0;

    size_t lines_total_ = 0;
    size_t lines_sent_ = 0;

    // 净作画时长与 ETA（对齐奎享 f.java:18-20 的 f/g/h 字段）。
    // draw_start_tick_ 在 StreamToGrbl 起点记；paused_accum_ms_ 在 WaitWhilePaused
    // 出口累加本次暂停段；ETA = 净时长 ÷ 进度（lines_sent/lines_total）。
    // 用 uint32_t 存 tick，避免头文件依赖 FreeRTOS 的 TickType_t。
    uint32_t draw_start_tick_ = 0;
    uint32_t paused_accum_ms_ = 0;
    uint32_t pause_segment_start_ = 0;
};

}  // namespace hutuji

#endif  // HUTUJI_JOB_H
