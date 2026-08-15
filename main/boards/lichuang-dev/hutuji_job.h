#ifndef HUTUJI_JOB_H
#define HUTUJI_JOB_H

#include "hutuji_recovery_core.h"

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
    /** 正常页尾专有：G1 归位（不触发换纸），随后才允许 ChangePaperAfterDraw。 */
    bool ReturnHomeAfterDraw();
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
     * 暂停超时（kMaxPauseMs）的统一收敛提交：锁内回滚 paused、置 abort 并推进
     * 纪元，然后通知用户并启动唯一 reset owner。WaitWhilePaused 与收分支的
     * 暂停冻结路径（R10-S3-01）共用，两处上限语义必须一致。
     */
    void CommitPauseTimeoutCancel();

    bool StartAbortResetTask();
    bool PerformAbortReset(bool wait_for_stream_quiescence, bool owner_claimed = false,
                           bool allow_unready_reconnect = false);
    /** 等已启动的 reset 恢复收敛，busy_ 在此之前不得释放。 */
    bool WaitForAbortReset();
    bool ResetAbortResetState();

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

    mutable std::mutex state_mutex_;
    // 协调 pause 与 SendLine：暂停字符发出后禁止再灌入新行。
    std::mutex stream_mutex_;
    std::string state_{"idle"};
    std::string last_error_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> paper_active_{false};
    std::atomic<bool> paused_{false};
    // pause/abort 提交纪元：RequestPause/RequestAbort、恢复失败回滚和暂停超时 abort
    // 均在 stream_mutex_ 内发布状态并递增。灌行候选在 stream_mutex_ 内捕获快照后
    // 解锁，执行 SendLine 前用 DecideStreamSend 复核「快照后无提交」；StreamToGrbl
    // 单行发送串行，因此该决策与暂停前「持锁直接检查」逐字等价。已有在途字节由
    // Grbl feed hold 与 planner 边界兜底（保持本轮出图画笔行为不变）。成功恢复不动
    // 纪元；恢复失败回滚 paused 时动纪元，拒绝恢复尝试期间形成的旧候选行。
    std::atomic<uint32_t> stream_control_epoch_{0};
    // 每个任务最多一个 reset owner；owner 收敛前 busy_ 始终保持 true。
    AbortResetOwner abort_reset_owner_;
    std::atomic<bool> abort_reset_worker_active_{false};
    std::atomic<uint32_t> abort_reset_session_{0};
    // 窗口退出时只有 Quiesced 能证明旧应答已全部消费；Failed 禁止 reset。
    std::atomic<StreamQuiescence> stream_quiescence_{StreamQuiescence::Idle};
    // abort owner 已用 fresh Hold:0/Idle 证明机器停稳；流任务可丢弃旧应答并发布 Quiesced。
    std::atomic<bool> abort_hold_confirmed_{false};
    // 重画：跳过下载/校验，直接复用 buffer_
    std::atomic<bool> repeat_mode_{false};
    // buffer_ 是否留存着可重画的 G-code（出图成功后不释放）
    std::atomic<bool> buffer_replayable_{false};
    // 试笔期间不接受暂停/恢复；abort 只置标志，不并发 reset 抢占 Z 运动应答。
    std::atomic<bool> pen_test_active_{false};

    bool stream_disconnected_ = false;
    // R21-F01：换纸播报一次性门控（多页连续换纸行不重复打扰）；仅任务线程读写。
    bool paper_change_notified_ = false;

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
