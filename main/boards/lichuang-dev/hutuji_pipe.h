#ifndef HUTUJI_PIPE_H
#define HUTUJI_PIPE_H

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace hutuji {

/** WaitResponse 结果：协议 §7 要求区分 error:8（Deferred）与其它失败。 */
enum class WaitResult {
    Ok = 0,
    Deferred,  // error:8，行已被丢弃，须重发
    Failed,    // 其它 error:NN
    Timeout,
};

/** Grbl 机器状态（由 `?` 状态报告或 ALARM 消息推断）。 */
enum class GrblState {
    Unknown = 0,
    Idle,
    Run,
    Hold,
    Jog,
    Alarm,
    Door,
    Check,
    Home,
    Sleep,
};

enum class PaperChangingState {
    Unknown = 0,
    Off,
    On,
};

/**
 * @brief hutuji 写字机哑管道（方案 E：WiFi Telnet TCP 客户端）
 *
 * 链路：写字机 Grbl_Esp32 Telnet:23 ← 实战派 S3 TCP 客户端。
 * WiFi 由 WifiBoard 管理；本类只做 TCP 连接/重连与行协议。
 * 单写者：所有往 Telnet 的字节（含 `?`/`!`/`0x18`）必须经本类 API。
 */
class Pipe {
public:
    static Pipe& GetInstance();

    void Start();

    /** 写一行普通命令（自动补 \\n）。未连接返回 false。吃 ok（单写者锁）。 */
    bool SendLine(const std::string& line);

    /**
     * 发实时字符（`?` / `!` / `0x18`），不吃 ok、不清应答位。
     * 仍占单写者锁，避免与 SendLine 字节交错。
     */
    bool SendRealtime(char ch);

    /**
     * 等待 Grbl 应答。timeout 内可被 abort 轮询打断（由调用方分段调用亦可）。
     * @param error_code 非空且 Failed/Deferred 时写入 NN；Timeout/Ok 时为 -1
     */
    WaitResult WaitResponse(uint32_t timeout_ms, std::string* response = nullptr,
                            int* error_code = nullptr);

    /** 兼容旧调用：仅 Ok→true。 */
    bool WaitOk(uint32_t timeout_ms, std::string* response = nullptr);

    /**
     * 消费一条应答（窗口化流控用，S1）。
     * 与 WaitResponse 的区别：语义是「这一条」的结果而非「有没有收到过」，
     * 多个 ok 连续到达不会被合并。ok 与 error 都计数（Grbl 官方 stream.py R2）。
     * @param error_code 非空且 Failed/Deferred 时写入 NN；Timeout/Ok 时为 -1
     */
    WaitResult TakeResponse(uint32_t timeout_ms, int* error_code = nullptr);

    /**
     * 窗口化流控开关（S1 设计文档 §4.1）。
     * 逐行模式（默认）：SendLine 前清空残留应答，与现状行为一致。
     * 窗口化模式：SendLine 不再清空——队列里的应答是「在途」的合法凭据。
     * S2/S3 阶段由 Job 侧打开；S1 保持默认 false。
     */
    void SetWindowed(bool enabled) { drain_on_send_.store(!enabled); }

    /** 丢弃队列内全部积压应答（连接重建 / 新任务开始时调用）。 */
    void DrainResponses();

    bool IsConnected() const { return connected_.load(); }
    bool IsReady() const { return ready_.load(); }
    bool IsAuthorized() const { return authorized_.load(); }
    /**
     * 绘图会话存续期间，重连只验 Telnet banner，不自动发送授权运动探针。
     * 任务层须先按 protocol §2.1 查询 Changing，再决定是否发受限 reset。
     */
    void SetTaskSessionActive(bool active) { task_session_active_.store(active); }
    uint32_t GetConnectionSequence() const { return connection_seq_.load(); }
    uint32_t GetResetBannerSequence() const { return reset_banner_seq_.load(); }
    uint32_t GetPaperStatusSequence() const { return paper_status_seq_.load(); }
    PaperChangingState GetPaperChangingState() const { return paper_changing_.load(); }

    /** 标记下一次 Grbl reset banner 来自本机 abort；仅该次允许自动 `$X` 恢复。 */
    void PrepareAbortReset() { abort_reset_pending_.store(true); }
    /** reset 字符未发出去时撤销许可，防止下一次无关 banner 被自动解锁。 */
    void CancelAbortReset() { abort_reset_pending_.store(false); }

    GrblState GetGrblState() const { return grbl_state_.load(); }
    uint32_t GetStatusReportSequence() const { return status_report_seq_.load(); }
    static const char* GrblStateName(GrblState s);

    void GetMachinePos(float& x, float& y, float& z) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        x = mpos_x_;
        y = mpos_y_;
        z = mpos_z_;
    }

    int GetAlarmCode() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return alarm_code_;
    }

    std::string GetLastLine() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_line_;
    }

    /**
     * 半关 socket 以唤醒阻塞 recv（不 release fd）。
     * 仅用于需要立刻打断接收泵的场景；真正 close 仍只在 pipe 任务内。
     */
    void ShutdownSocket();

private:
    enum class PeerCheck {
        Valid,
        ClosedBeforeBanner,
        Invalid,
    };

    enum class AuthProbeStage {
        Idle,
        WaitingAbortUnlockOk,
        WaitingAbortLiftOk,
        WaitingAbortLiftIdle,
        WaitingBuildInfoOk,
        WaitingLiftOk,
        WaitingPosition,
        WaitingMotionReply,
        Complete,
        Failed,
    };

    Pipe() = default;
    ~Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    static void PipeTaskEntry(void* arg);
    void PipeTask();
    bool ConnectOnce();
    bool TryConnect(uint32_t ip_addr, int timeout_ms);  // sock_mutex_ 已持有
    /**
     * 校验对端确实是 Grbl 写字机（读 Telnet 连接 banner）。sock_mutex_ 已持有。
     * 缓存命中与子网扫描共用此校验：只认真写字机，堵住「任意 :23 主机顶替」。
     */
    PeerCheck VerifyGrblPeer(int sock, int timeout_ms);
    void CloseSocket();
    bool SendRawLocked(const char* data, size_t len);  // 调用方已持 write_mutex_

    void OnRxData(const uint8_t* data, size_t data_len);
    void ProcessLine(const std::string& line);
    void ParseStatusReport(const std::string& line);
    void NotifyCloud(const std::string& message);
    static int ParseErrorCode(const std::string& line);
    bool HandleAuthProbeResponse(WaitResult result, int error_code);

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> authorized_{false};
    std::atomic<bool> abort_reset_pending_{false};
    std::atomic<bool> task_session_active_{false};
    std::atomic<GrblState> grbl_state_{GrblState::Unknown};
    std::atomic<uint32_t> status_report_seq_{0};
    std::atomic<uint32_t> connection_seq_{0};
    std::atomic<uint32_t> reset_banner_seq_{0};
    std::atomic<uint32_t> paper_status_seq_{0};
    std::atomic<PaperChangingState> paper_changing_{PaperChangingState::Unknown};
    // Grbl 的授权日志只发 CLIENT_SERIAL，Telnet `$I` 看不到。连接后用抬笔状态下的
    // G53 零位移运动探测 error:110；探测完成前 ready_ 保持 false，禁止绘图抢跑。
    AuthProbeStage auth_probe_stage_ = AuthProbeStage::Idle;

    TaskHandle_t pipe_task_ = nullptr;

    /**
     * 应答队列：每条 ok/error 入队一次，深度 kRespQueueDepth。
     * 取代原 EventGroup 单 bit —— bit 只能表达「有没有」，多个 ok 连续到达
     * 会被合并成一个，窗口化流控的在途计数会永久漂移最终死锁。
     */
    QueueHandle_t resp_queue_ = nullptr;

    /** 逐行模式下 SendLine 前是否清残留应答（默认 true；窗口化模式置 false）。 */
    std::atomic<bool> drain_on_send_{true};

    /** 单条应答的载荷。 */
    struct RespItem {
        WaitResult result;
        int error_code;
    };

    /** 入队一条应答（仅 ProcessLine 的 ok/error 分支调用）。队列满时丢最老。 */
    void PushResponse(WaitResult result, int error_code);

    // 单写者：所有发送（行/实时）串行化
    std::mutex write_mutex_;
    std::mutex sock_mutex_;
    int sock_ = -1;

    std::string rx_buffer_;

    mutable std::mutex state_mutex_;
    std::string last_line_;
    std::string last_response_;
    int last_error_code_ = -1;
    float mpos_x_ = 0, mpos_y_ = 0, mpos_z_ = 0;
    int alarm_code_ = 0;

    char resolved_ip_[16] = {};
    // Grbl 仅允许一个 Telnet 客户端。S3 重启后旧半开连接尚未回收时，
    // 新连接会 TCP 成功后立即关闭；这不是缓存 IP 失真，先快速重试再回落扫描。
    uint8_t cached_slot_busy_count_ = 0;
};

}  // namespace hutuji

#endif  // HUTUJI_PIPE_H
