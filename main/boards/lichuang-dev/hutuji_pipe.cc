#include "hutuji_pipe.h"

#include <cerrno>
#include <cstring>

#include <esp_log.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "HutujiPipe"

// ===== 联调配置：写字机 Grbl_Esp32 Telnet 地址（联调时改成实际固定 IP）=====
#define HUTUJI_PIPE_HOST "192.168.1.50"
#define HUTUJI_PIPE_PORT 23

namespace hutuji {

namespace {
// Grbl 应答事件位
constexpr EventBits_t kResponseOkBit = (1 << 0);
constexpr EventBits_t kResponseErrorBit = (1 << 1);

constexpr size_t kRxLineMax = 512;        // 单行聚合上限，防异常字节流撑爆内存
constexpr uint32_t kBackoffInitMs = 1000; // 重连退避：1s/2s/4s…
constexpr uint32_t kBackoffMaxMs = 30000; // …上限 30s
} // namespace

Pipe& Pipe::GetInstance() {
    static Pipe instance;
    return instance;
}

void Pipe::Start() {
    // 幂等：只允许启动一次
    if (started_.exchange(true)) {
        return;
    }

    response_events_ = xEventGroupCreate();
    configASSERT(response_events_);

    // WiFi 由 WifiBoard 管理，本任务只做 TCP 层连接与退避重连
    BaseType_t ok = xTaskCreate(PipeTaskEntry, "hutuji_tcp", 4096, this, 10, &pipe_task_);
    configASSERT(ok == pdTRUE);

    ESP_LOGI(TAG, "hutuji Telnet 哑管道已启动（目标 %s:%d）", HUTUJI_PIPE_HOST, HUTUJI_PIPE_PORT);
}

void Pipe::PipeTaskEntry(void* arg) {
    static_cast<Pipe*>(arg)->PipeTask();
}

void Pipe::PipeTask() {
    uint32_t backoff_ms = kBackoffInitMs;
    while (true) {
        if (!ConnectOnce()) {
            ESP_LOGW(TAG, "连接 %s:%d 失败，%ums 后重试", HUTUJI_PIPE_HOST, HUTUJI_PIPE_PORT,
                (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff_ms * 2;
            continue;
        }
        backoff_ms = kBackoffInitMs;  // 连接成功，退避归零
        connected_.store(true);
        ESP_LOGI(TAG, "已连接写字机 Telnet（%s:%d）", HUTUJI_PIPE_HOST, HUTUJI_PIPE_PORT);

        // 就绪探活：$I 查询构建信息，等 "[VER:" 置 ready_
        SendRaw("$I\n", 3);

        // 接收泵：阻塞读 → 行聚合 → 应答解析
        uint8_t buf[256];
        while (true) {
            int len = recv(sock_, buf, sizeof(buf), 0);
            if (len <= 0) {
                // 0 = 对端正常关闭，<0 = 错误，都走重连
                if (len < 0) {
                    ESP_LOGW(TAG, "recv 错误: errno=%d (%s)", errno, strerror(errno));
                }
                break;
            }
            OnRxData(buf, static_cast<size_t>(len));
        }

        CloseSocket();
        connected_.store(false);
        ready_.store(false);
        rx_buffer_.clear();
        xEventGroupClearBits(response_events_, kResponseOkBit | kResponseErrorBit);
        ESP_LOGW(TAG, "写字机 Telnet 已断开，%ums 后重连", (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

bool Pipe::ConnectOnce() {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        ESP_LOGE(TAG, "socket 创建失败: errno=%d (%s)", errno, strerror(errno));
        return false;
    }

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(HUTUJI_PIPE_PORT);
    dest.sin_addr.s_addr = inet_addr(HUTUJI_PIPE_HOST);

    // 同步 connect；对端不可达时会阻塞到 lwip 超时，M1 可接受
    if (connect(s, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) != 0) {
        close(s);
        return false;
    }

    // 可选保活：尽快发现对端断电/断网
    int keepalive = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    std::lock_guard<std::mutex> lock(sock_mutex_);
    sock_ = s;
    return true;
}

void Pipe::CloseSocket() {
    std::lock_guard<std::mutex> lock(sock_mutex_);
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

bool Pipe::SendRaw(const char* data, size_t len) {
    std::lock_guard<std::mutex> lock(sock_mutex_);
    if (sock_ < 0) {
        return false;
    }
    size_t sent = 0;
    while (sent < len) {
        // ESP-IDF lwip 不产生 SIGPIPE，无需 MSG_NOSIGNAL；断连靠返回值判断
        int n = send(sock_, data + sent, len - sent, 0);
        if (n < 0) {
            ESP_LOGE(TAG, "send 失败: errno=%d (%s)", errno, strerror(errno));
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void Pipe::OnRxData(const uint8_t* data, size_t data_len) {
    rx_buffer_.append(reinterpret_cast<const char*>(data), data_len);

    // 异常保护：长时间没有换行说明对端不按行协议，截断缓冲
    if (rx_buffer_.size() > kRxLineMax) {
        ESP_LOGW(TAG, "接收行超长，丢弃 %zu 字节", rx_buffer_.size());
        rx_buffer_.clear();
    }

    size_t pos;
    while ((pos = rx_buffer_.find('\n')) != std::string::npos) {
        std::string line = rx_buffer_.substr(0, pos);
        rx_buffer_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            ProcessLine(line);
        }
    }
}

void Pipe::ProcessLine(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_line_ = line;
    }
    ESP_LOGI(TAG, "<- %s", line.c_str());

    // 探活应答（如 "[VER:1.3a.20240101:...]"）：标记对端就绪
    if (line.rfind("[VER:", 0) == 0) {
        ready_.store(true);
        ESP_LOGI(TAG, "Grbl 探活成功（%s）", line.c_str());
        return;
    }

    // 就绪标志（如 "Grbl 1.3a ['$' for help]"）：运行中出现 = 对端复位，重置状态并重新探活
    if (line.rfind("Grbl ", 0) == 0) {
        ready_.store(false);
        xEventGroupClearBits(response_events_, kResponseOkBit | kResponseErrorBit);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_.clear();
        }
        ESP_LOGW(TAG, "检测到对端复位，重新探活");
        SendRaw("$I\n", 3);
        return;
    }

    if (line == "ok") {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
        }
        xEventGroupSetBits(response_events_, kResponseOkBit);
    } else if (line.rfind("error", 0) == 0) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
        }
        xEventGroupSetBits(response_events_, kResponseErrorBit);
        ESP_LOGW(TAG, "Grbl 应答错误: %s", line.c_str());
    }
}

bool Pipe::SendLine(const std::string& line) {
    if (!connected_.load()) {
        ESP_LOGW(TAG, "TCP 未连接，丢弃发送: %s", line.c_str());
        return false;
    }

    // 发送前清掉上一轮应答位，保证 WaitOk 等的是本次应答
    xEventGroupClearBits(response_events_, kResponseOkBit | kResponseErrorBit);

    std::string payload = line;
    payload.push_back('\n');
    if (!SendRaw(payload.data(), payload.size())) {
        ESP_LOGE(TAG, "发送失败: %s", line.c_str());
        return false;
    }
    ESP_LOGI(TAG, "-> %s", line.c_str());
    return true;
}

bool Pipe::WaitOk(uint32_t timeout_ms, std::string* response) {
    if (response_events_ == nullptr) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(response_events_,
        kResponseOkBit | kResponseErrorBit, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (response != nullptr) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        *response = last_response_;
    }
    if (bits & kResponseOkBit) {
        return true;
    }
    if ((bits & (kResponseOkBit | kResponseErrorBit)) == 0) {
        ESP_LOGW(TAG, "等待 Grbl 应答超时 (%ums)", (unsigned)timeout_ms);
    }
    return false;
}

} // namespace hutuji
