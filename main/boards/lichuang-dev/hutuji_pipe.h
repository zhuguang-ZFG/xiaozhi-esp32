#ifndef HUTUJI_PIPE_H
#define HUTUJI_PIPE_H

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

namespace hutuji {

/**
 * @brief hutuji 写字机哑管道（方案 E：WiFi Telnet TCP 客户端）
 *
 * 链路：写字机 Grbl_Esp32 内置 WebUI 的 Telnet 服务（端口 23，STA 固定 IP）
 *       ← 实战派 S3 作为 TCP 客户端直连。字节流与串口完全同构
 *       （行协议、ok/error:NN、banner、实时字符一致）。
 * WiFi 本身由 xiaozhi 的 WifiBoard 管理，本类只做 TCP 层连接/重连，
 * WiFi 未就绪时 connect 自然失败走退避重试即可。
 * G-code 由云端生成，本类只做下载结果的转发与应答解析（M1 不含下载）。
 */
class Pipe {
public:
    static Pipe& GetInstance();

    // 创建连接管理任务；幂等，重复调用直接返回
    void Start();

    // 写一行 G-code（自动补 \n）。TCP 未连接返回 false
    bool SendLine(const std::string& line);

    // 等待 Grbl 应答：收到 "ok" 返回 true，收到 "error:" / 超时返回 false。
    // response 非空时回传实际应答行
    bool WaitOk(uint32_t timeout_ms, std::string* response = nullptr);

    bool IsConnected() const { return connected_.load(); }  // TCP 已连接
    bool IsReady() const { return ready_.load(); }          // 探活收到 "[VER:"

    std::string GetLastLine() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_line_;
    }

private:
    Pipe() = default;
    ~Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    static void PipeTaskEntry(void* arg);
    void PipeTask();            // 连接管理：connect → 探活 → recv 泵 → 断线退避重连
    bool ConnectOnce();         // 建立一次 TCP 连接（同步），成功置 sock_
    void CloseSocket();
    bool SendRaw(const char* data, size_t len);  // 内部发送（含部分写循环）

    void OnRxData(const uint8_t* data, size_t data_len);
    void ProcessLine(const std::string& line);

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};  // TCP 已连接
    std::atomic<bool> ready_{false};      // 探活成功（收到 "[VER:"）；banner 出现视为对端复位

    TaskHandle_t pipe_task_ = nullptr;
    EventGroupHandle_t response_events_ = nullptr;  // Grbl 应答信号

    // sock_ 由 pipe 任务创建/关闭，SendLine 经 sock_mutex_ 保护访问
    std::mutex sock_mutex_;
    int sock_ = -1;

    // 接收行聚合缓冲（仅 pipe 任务访问）
    std::string rx_buffer_;

    mutable std::mutex state_mutex_;
    std::string last_line_;      // 最近收到的一行
    std::string last_response_;  // 最近一次 ok/error 应答原文
};

} // namespace hutuji

#endif // HUTUJI_PIPE_H
