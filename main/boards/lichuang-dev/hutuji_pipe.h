#ifndef HUTUJI_PIPE_H
#define HUTUJI_PIPE_H

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

namespace hutuji {

/** WaitResponse 结果：协议 §7 要求区分 error:8（Deferred）与其它失败。 */
enum class WaitResult {
    Ok = 0,
    Deferred,  // error:8，行已被丢弃，须重发
    Failed,    // 其它 error:NN
    Timeout,
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

    bool IsConnected() const { return connected_.load(); }
    bool IsReady() const { return ready_.load(); }
    bool IsAuthorized() const { return authorized_.load(); }

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
    Pipe() = default;
    ~Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    static void PipeTaskEntry(void* arg);
    void PipeTask();
    bool ConnectOnce();
    bool TryConnect(uint32_t ip_addr, int timeout_ms);  // sock_mutex_ 已持有
    void CloseSocket();
    bool SendRawLocked(const char* data, size_t len);  // 调用方已持 write_mutex_

    void OnRxData(const uint8_t* data, size_t data_len);
    void ProcessLine(const std::string& line);
    static int ParseErrorCode(const std::string& line);

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> authorized_{false};

    TaskHandle_t pipe_task_ = nullptr;
    EventGroupHandle_t response_events_ = nullptr;

    // 单写者：所有发送（行/实时）串行化
    std::mutex write_mutex_;
    std::mutex sock_mutex_;
    int sock_ = -1;

    std::string rx_buffer_;

    mutable std::mutex state_mutex_;
    std::string last_line_;
    std::string last_response_;
    int last_error_code_ = -1;

    char resolved_ip_[16] = {};
};

} // namespace hutuji

#endif // HUTUJI_PIPE_H
