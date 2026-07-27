#include "hutuji_pipe.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

#include "application.h"
#include "lwip/sockets.h"

#define TAG "HutujiPipe"

#define HUTUJI_PIPE_PORT 23
#define HUTUJI_PIPE_SCAN_TIMEOUT_MS 50
#define HUTUJI_PIPE_CACHED_TIMEOUT_MS 2000

#define HUTUJI_NVS_NS "hutuji_pipe"
#define HUTUJI_NVS_KEY_IP "grbl_ip"

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
constexpr int kPollIntervalSec = 3;

uint32_t LoadCachedIp() {
    nvs_handle_t h;
    uint32_t ip = 0;
    if (nvs_open(HUTUJI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, HUTUJI_NVS_KEY_IP, &ip);
        nvs_close(h);
    }
    return ip;
}

void SaveCachedIp(uint32_t ip) {
    nvs_handle_t h;
    if (nvs_open(HUTUJI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, HUTUJI_NVS_KEY_IP, ip);
        nvs_commit(h);
        nvs_close(h);
    }
}
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

    ESP_LOGI(TAG, "hutuji Telnet 哑管道已启动（端口 %d）", HUTUJI_PIPE_PORT);
}

void Pipe::PipeTaskEntry(void* arg) {
    static_cast<Pipe*>(arg)->PipeTask();
}

void Pipe::PipeTask() {
    while (esp_netif_get_nr_of_ifs() == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "网络接口就绪，开始连接写字机");

    uint32_t backoff_ms = kBackoffInitMs;
    while (true) {
        if (!ConnectOnce()) {
            ESP_LOGW(TAG, "连接写字机失败，%ums 后重试", (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > kBackoffMaxMs) ? kBackoffMaxMs : backoff_ms * 2;
            continue;
        }
        backoff_ms = kBackoffInitMs;
        connected_.store(true);
        ESP_LOGI(TAG, "已连接写字机 Telnet（%s:%d）", resolved_ip_, HUTUJI_PIPE_PORT);
        NotifyCloud(std::string("写字机已连接 (") + resolved_ip_ + ")");

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            SendRawLocked("$I\n", 3);
        }

        uint8_t buf[256];
        while (true) {
            int len = recv(sock_, buf, sizeof(buf), 0);
            if (len > 0) {
                OnRxData(buf, static_cast<size_t>(len));
            } else if (len == 0) {
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // recv 超时：发 ? 轮询 Grbl 状态
                    std::lock_guard<std::mutex> wlock(write_mutex_);
                    SendRawLocked("?", 1);
                    continue;
                }
                ESP_LOGW(TAG, "recv 错误: errno=%d (%s)", errno, strerror(errno));
                break;
            }
        }

        CloseSocket();
        connected_.store(false);
        ready_.store(false);
        authorized_.store(false);
        grbl_state_.store(GrblState::Unknown);
        NotifyCloud("写字机连接断开");
        rx_buffer_.clear();
        xEventGroupClearBits(response_events_, kResponseOkBit | kResponseErrorBit);
        ESP_LOGW(TAG, "写字机 Telnet 已断开，%ums 后重连", (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

bool Pipe::TryConnect(uint32_t ip_addr, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;

    int old_flags = lwip_fcntl(s, F_GETFL, 0);
    lwip_fcntl(s, F_SETFL, old_flags | O_NONBLOCK);

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(HUTUJI_PIPE_PORT);
    dest.sin_addr.s_addr = ip_addr;

    int ret = connect(s, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    int conn_errno = errno;

    if (ret == 0) {
        // 立即成功（极少见）
    } else if (conn_errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        int sel = select(s + 1, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            close(s);
            return false;
        }
        int so_err = 0;
        socklen_t elen = sizeof(so_err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &elen);
        if (so_err != 0) {
            close(s);
            return false;
        }
    } else {
        close(s);
        return false;
    }

    lwip_fcntl(s, F_SETFL, old_flags & ~O_NONBLOCK);
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    inet_ntoa_r(dest.sin_addr, resolved_ip_, sizeof(resolved_ip_));
    sock_ = s;
    return true;
}

bool Pipe::ConnectOnce() {
    // 等 DHCP 拿到 IP 再尝试（避免 WiFi 连接前的无效重试）
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        return false;
    }

    bool found = false;
    uint32_t cached_ip = LoadCachedIp();

    // 0) NVS 缓存（上次发现的写字机，2s 超时容忍短暂不可达）
    if (cached_ip != 0) {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        if (TryConnect(cached_ip, HUTUJI_PIPE_CACHED_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "缓存 IP 命中: %s", resolved_ip_);
            found = true;
        }
    }

    // 1) 子网扫描 Telnet:23（.1 ~ .254，50ms/地址）
    if (!found) {
        uint32_t base = ip_info.ip.addr & ip_info.netmask.addr;
        uint32_t self = ip_info.ip.addr;
        ESP_LOGI(TAG, "扫描子网寻找写字机 Telnet:23...");
        std::lock_guard<std::mutex> lock(sock_mutex_);
        for (int host = 1; host < 255; ++host) {
            uint32_t target = base | htonl(host);
            if (target == self) continue;
            if (target == cached_ip) continue;
            if (TryConnect(target, HUTUJI_PIPE_SCAN_TIMEOUT_MS)) {
                ESP_LOGI(TAG, "子网扫描发现写字机 %s", resolved_ip_);
                found = true;
                break;
            }
        }
    }

    if (!found) return false;

    // 缓存 IP 到 NVS（下次启动秒连，IP 未变时不写）
    struct sockaddr_in peer = {};
    socklen_t plen = sizeof(peer);
    if (getpeername(sock_, reinterpret_cast<struct sockaddr*>(&peer), &plen) == 0 &&
        peer.sin_addr.s_addr != cached_ip) {
        SaveCachedIp(peer.sin_addr.s_addr);
        ESP_LOGI(TAG, "写字机 IP 已缓存到 NVS");
    }

    // keepalive: 10s 空闲 + 3s×3 次探测 ≈ 19s 发现死连
    int keepalive = 1;
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    int keep_idle = kKeepIdleSec;
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    int keep_intvl = kKeepIntvlSec;
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
    int keep_cnt = kKeepCnt;
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));

    // recv 超时驱动周期轮询：无数据 3s 后 recv 返回 EAGAIN，PipeTask 发 `?`
    struct timeval recv_tv = { .tv_sec = kPollIntervalSec, .tv_usec = 0 };
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
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

    // `?` 状态报告：<State|MPos:X,Y,Z|...>
    if (!line.empty() && line.front() == '<' && line.back() == '>') {
        ParseStatusReport(line);
        return;
    }

    // ALARM:N 消息
    if (line.rfind("ALARM:", 0) == 0) {
        int code = std::atoi(line.c_str() + 6);
        GrblState prev = grbl_state_.exchange(GrblState::Alarm);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            alarm_code_ = code;
        }
        ESP_LOGW(TAG, "Grbl 报警: ALARM:%d", code);
        if (prev != GrblState::Alarm) {
            float ax, ay, az;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                ax = mpos_x_; ay = mpos_y_; az = mpos_z_;
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "写字机报警 ALARM:%d 位置 X=%.1f Y=%.1f Z=%.1f", code, ax, ay, az);
            NotifyCloud(buf);
        }
        return;
    }

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

void Pipe::ParseStatusReport(const std::string& line) {
    // <State|MPos:X,Y,Z|Bf:N,N|FS:N,N|...>
    std::string content = line.substr(1, line.size() - 2);

    size_t pipe_pos = content.find('|');
    std::string state_str = (pipe_pos != std::string::npos)
        ? content.substr(0, pipe_pos) : content;

    // Hold:0 / Alarm:1 等带子码
    int sub_code = 0;
    size_t colon = state_str.find(':');
    if (colon != std::string::npos) {
        sub_code = std::atoi(state_str.c_str() + colon + 1);
        state_str = state_str.substr(0, colon);
    }

    GrblState gs = GrblState::Unknown;
    if (state_str == "Idle")       gs = GrblState::Idle;
    else if (state_str == "Run")   gs = GrblState::Run;
    else if (state_str == "Hold")  gs = GrblState::Hold;
    else if (state_str == "Jog")   gs = GrblState::Jog;
    else if (state_str == "Alarm") gs = GrblState::Alarm;
    else if (state_str == "Door")  gs = GrblState::Door;
    else if (state_str == "Check") gs = GrblState::Check;
    else if (state_str == "Home")  gs = GrblState::Home;
    else if (state_str == "Sleep") gs = GrblState::Sleep;

    GrblState prev = grbl_state_.exchange(gs);
    if (prev != gs) {
        ESP_LOGI(TAG, "Grbl 状态: %s -> %s", GrblStateName(prev), GrblStateName(gs));
        float nx, ny, nz;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            nx = mpos_x_; ny = mpos_y_; nz = mpos_z_;
        }
        char buf[128];
        if (gs == GrblState::Alarm) {
            std::snprintf(buf, sizeof(buf),
                "写字机报警 ALARM:%d 位置 X=%.1f Y=%.1f Z=%.1f", sub_code, nx, ny, nz);
            NotifyCloud(buf);
        } else if (prev == GrblState::Run && gs == GrblState::Idle) {
            std::snprintf(buf, sizeof(buf),
                "写字机运动完成 (Run→Idle) 停在 X=%.1f Y=%.1f", nx, ny);
            NotifyCloud(buf);
        } else if (gs == GrblState::Hold) {
            std::snprintf(buf, sizeof(buf),
                "写字机暂停 (Hold) 位置 X=%.1f Y=%.1f", nx, ny);
            NotifyCloud(buf);
        }
    }

    // MPos
    size_t mpos = content.find("MPos:");
    if (mpos != std::string::npos) {
        float x = 0, y = 0, z = 0;
        if (std::sscanf(content.c_str() + mpos, "MPos:%f,%f,%f", &x, &y, &z) == 3) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mpos_x_ = x; mpos_y_ = y; mpos_z_ = z;
        }
    }

    if (gs == GrblState::Alarm) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        alarm_code_ = sub_code;
    }
}

const char* Pipe::GrblStateName(GrblState s) {
    switch (s) {
        case GrblState::Unknown: return "Unknown";
        case GrblState::Idle:    return "Idle";
        case GrblState::Run:     return "Run";
        case GrblState::Hold:    return "Hold";
        case GrblState::Jog:     return "Jog";
        case GrblState::Alarm:   return "Alarm";
        case GrblState::Door:    return "Door";
        case GrblState::Check:   return "Check";
        case GrblState::Home:    return "Home";
        case GrblState::Sleep:   return "Sleep";
        default:                 return "?";
    }
}

void Pipe::NotifyCloud(const std::string& message) {
    ESP_LOGI(TAG, "notify: %s", message.c_str());
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "notifications/message");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "level", "info");
    cJSON_AddStringToObject(params, "data", message.c_str());
    cJSON_AddItemToObject(root, "params", params);
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str) {
        Application::GetInstance().SendMcpMessage(str);
        cJSON_free(str);
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
