#include "hutuji_pipe.h"

#include <cerrno>
#include <cstring>

#include <esp_log.h>
#include <esp_netif.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "HutujiPipe"

// ===== 联调配置：写字机 Grbl_Esp32 Telnet 地址（联调时改成实际固定 IP）=====
#define HUTUJI_PIPE_HOST "192.168.1.50"
#define HUTUJI_PIPE_PORT 23

namespace hutuji {

namespace {
constexpr EventBits_t kResponseOkBit = (1 << 0);
constexpr EventBits_t kResponseErrorBit = (1 << 1);

constexpr size_t kRxLineMax = 512;
constexpr uint32_t kBackoffInitMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

constexpr int kKeepIdleSec = 10;
constexpr int kKeepIntvlSec = 3;
constexpr int kKeepCnt = 3;
} // namespace

Pipe& Pipe::GetInstance() {
    static Pipe instance;
    return instance;
}

void Pipe::Start() {
    if (started_.exchange(true)) {
        return;
    }

    response_events_ = xEventGroupCreate();
    configASSERT(response_events_);

    BaseType_t ok = xTaskCreate(PipeTaskEntry, "hutuji_tcp", 4096, this, 10, &pipe_task_);
    configASSERT(ok == pdTRUE);

    ESP_LOGI(TAG, "hutuji Telnet 哑管道已启动（目标 %s:%d）", HUTUJI_PIPE_HOST, HUTUJI_PIPE_PORT);
}

void Pipe::PipeTaskEntry(void* arg) {
    static_cast<Pipe*>(arg)->PipeTask();
}

void Pipe::PipeTask() {
    // 等 lwip 协议栈就绪（WiFi 初始化在 InitializeTools 之后）
    while (esp_netif_get_nr_of_ifs() == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "网络接口就绪，开始连接写字机");

    uint32_t backoff_ms = kBackoffInitMs;
    while (true) {
        if (!ConnectOnce()) {
            ESP_LOGW(TAG, "连接 %s:%d 失败，%ums 后重试", HUTUJI_PIPE_HOST, HUTUJI_PIPE_PORT,
                     (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff_ms * 2;
            continue;
        }
        backoff_ms = kBackoffInitMs;
        connected_.store(true);
        ESP_LOGI(TAG, "已连接写字机 Telnet（%s:%d）", HUTUJI_PIPE_HOST, HUTUJI_PIPE_PORT);

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            SendRawLocked("$I\n", 3);
        }

        uint8_t buf[256];
        while (true) {
            int len = recv(sock_, buf, sizeof(buf), 0);
            if (len <= 0) {
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
        // 授权随会话：断连后需重新从应答推断
        authorized_.store(false);
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

    if (connect(s, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) != 0) {
        close(s);
        return false;
    }

    int keepalive = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    int keep_idle = kKeepIdleSec;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    int keep_intvl = kKeepIntvlSec;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
    int keep_cnt = kKeepCnt;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));

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

void Pipe::ShutdownSocket() {
    std::lock_guard<std::mutex> lock(sock_mutex_);
    if (sock_ >= 0) {
        shutdown(sock_, SHUT_RDWR);
    }
}

bool Pipe::SendRawLocked(const char* data, size_t len) {
    std::lock_guard<std::mutex> lock(sock_mutex_);
    if (sock_ < 0) {
        return false;
    }
    size_t sent = 0;
    while (sent < len) {
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

    // 先拆完整行，再丢超长残段——避免同分片里的 ok 被 clear 掉（M2 / protocol §3）
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
    if (rx_buffer_.size() > kRxLineMax) {
        ESP_LOGW(TAG, "接收残段超长，丢弃 %zu 字节", rx_buffer_.size());
        rx_buffer_.clear();
    }
}

int Pipe::ParseErrorCode(const std::string& line) {
    // "error:8" / "error:110" / 旧式 "error 8"
    if (line.rfind("error", 0) != 0) {
        return -1;
    }
    size_t i = 5;
    while (i < line.size() && (line[i] == ':' || line[i] == ' ')) {
        ++i;
    }
    if (i >= line.size() || line[i] < '0' || line[i] > '9') {
        return -1;
    }
    int code = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        code = code * 10 + (line[i] - '0');
        ++i;
    }
    return code;
}

void Pipe::ProcessLine(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_line_ = line;
    }
    ESP_LOGI(TAG, "<- %s", line.c_str());

    if (line.find("[License] Authorized") != std::string::npos) {
        authorized_.store(true);
    }

    if (line.rfind("[VER:", 0) == 0) {
        ready_.store(true);
        ESP_LOGI(TAG, "Grbl 探活成功（%s）", line.c_str());
        return;
    }

    if (line.rfind("Grbl ", 0) == 0) {
        ready_.store(false);
        authorized_.store(false);
        xEventGroupClearBits(response_events_, kResponseOkBit | kResponseErrorBit);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_.clear();
            last_error_code_ = -1;
        }
        ESP_LOGW(TAG, "检测到对端复位，重新探活");
        std::lock_guard<std::mutex> lock(write_mutex_);
        SendRawLocked("$I\n", 3);
        return;
    }

    if (line == "ok") {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
            last_error_code_ = -1;
        }
        xEventGroupSetBits(response_events_, kResponseOkBit);
    } else if (line.rfind("error", 0) == 0) {
        int code = ParseErrorCode(line);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
            last_error_code_ = code;
        }
        xEventGroupSetBits(response_events_, kResponseErrorBit);
        ESP_LOGW(TAG, "Grbl 应答错误: %s (code=%d)", line.c_str(), code);
    }
}

bool Pipe::SendLine(const std::string& line) {
    if (!connected_.load()) {
        ESP_LOGW(TAG, "TCP 未连接，丢弃发送: %s", line.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(write_mutex_);
    xEventGroupClearBits(response_events_, kResponseOkBit | kResponseErrorBit);

    std::string payload = line;
    payload.push_back('\n');
    if (!SendRawLocked(payload.data(), payload.size())) {
        ESP_LOGE(TAG, "发送失败: %s", line.c_str());
        return false;
    }
    ESP_LOGI(TAG, "-> %s", line.c_str());
    return true;
}

bool Pipe::SendRealtime(char ch) {
    if (!connected_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    return SendRawLocked(&ch, 1);
}

WaitResult Pipe::WaitResponse(uint32_t timeout_ms, std::string* response, int* error_code) {
    if (response_events_ == nullptr) {
        return WaitResult::Timeout;
    }
    EventBits_t bits = xEventGroupWaitBits(response_events_,
                                           kResponseOkBit | kResponseErrorBit, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    int code = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (response != nullptr) {
            *response = last_response_;
        }
        code = last_error_code_;
    }
    if (error_code != nullptr) {
        *error_code = code;
    }

    if (bits & kResponseOkBit) {
        return WaitResult::Ok;
    }
    if (bits & kResponseErrorBit) {
        if (code == 8) {
            return WaitResult::Deferred;
        }
        return WaitResult::Failed;
    }
    ESP_LOGW(TAG, "等待 Grbl 应答超时 (%ums)", (unsigned)timeout_ms);
    return WaitResult::Timeout;
}

bool Pipe::WaitOk(uint32_t timeout_ms, std::string* response) {
    return WaitResponse(timeout_ms, response, nullptr) == WaitResult::Ok;
}

} // namespace hutuji
