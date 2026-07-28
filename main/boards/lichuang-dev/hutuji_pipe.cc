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
// 应答队列深度：窗口化流控（设计见 docs/design/p2-windowed-flow-control.md）下
// 在途可达 ~21 行，队列须能缓冲一串连续到达的应答而不丢。取 32 留余量。
constexpr UBaseType_t kRespQueueDepth = 32;

constexpr size_t kRxLineMax = 512;
constexpr uint32_t kBackoffInitMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

constexpr int kKeepIdleSec = 10;
constexpr int kKeepIntvlSec = 3;
constexpr int kKeepCnt = 3;
constexpr int kPollIntervalSec = 3;

// 连续多少次 recv 超时（每次都发过 `?`）仍收不到任何字节，就判定链路已死。
// TCP keepalive（10/3/3 ≈ 19s）只能发现「TCP 层断了」；若对端 TCP 栈活着而 Grbl
// 主循环假死（不回 `?`、不回状态行），keepalive 不触发，原先的无条件 continue 会
// 永远转圈。参考奎享同类兜底：Grbl WiFi 侧 1s 发 `?`、累计 20 拍收不到状态行即断链
// （machine/grbl/a.java:382-404），串口侧两级静默 5s+5s（machine/b/b.java:20-28）。
// 本机 kPollIntervalSec=3s，取 7 次 ≈ 21s，与 keepalive 的 19s 同量级互为补充。
constexpr int kSilentPollLimit = 7;

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

    // 应答队列取代原 EventGroup 单 bit：窗口化流控必须知道「收到了几个 ok」，
    // EventGroup 会把连续到达的多个 ok 合并成一个 bit，在途计数会永久漂移。
    resp_queue_ = xQueueCreate(kRespQueueDepth, sizeof(RespItem));
    configASSERT(resp_queue_);

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
        // 连续「探活已发但一个字节都没回」的次数。任何数据到达即归零。
        int silent_polls = 0;
        while (true) {
            int len = recv(sock_, buf, sizeof(buf), 0);
            if (len > 0) {
                silent_polls = 0;
                OnRxData(buf, static_cast<size_t>(len));
            } else if (len == 0) {
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // recv 超时：发 ? 轮询 Grbl 状态。
                    // TCP keepalive 只能发现「TCP 层死了」；若对端协议栈活着而 Grbl
                    // 主循环假死（不回 `?`），keepalive 不触发，这里必须自己判死，
                    // 否则本循环会永远转圈。参考奎享 grbl/a.java:382-404 的同类机制。
                    if (++silent_polls >= kSilentPollLimit) {
                        ESP_LOGW(TAG, "连续 %d 次探活无应答（约 %ds），判定链路假死",
                                 silent_polls, silent_polls * kPollIntervalSec);
                        break;
                    }
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
        // 断连必须清空应答队列：上次连接遗留的 ok 若留到下次，会被当成本次命令的
        // 应答，窗口化流控下等于在途计数凭空少一格（最终死锁）。
        DrainResponses();
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
        // 对端软复位会丢弃其内部排队状态，我方遗留应答全部失效。
        DrainResponses();
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

    // 只有 `ok` 与 `error:NN` 入应答队列。banner / `[VER:` / `<...>` 状态行
    // 都在上面提前 return —— 对应官方 stream.py 的「非应答行不消耗窗口」（R3）。
    if (line == "ok") {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
            last_error_code_ = -1;
        }
        PushResponse(WaitResult::Ok, -1);
    } else if (line.rfind("error", 0) == 0) {
        int code = ParseErrorCode(line);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
            last_error_code_ = code;
        }
        // error:8 = 换纸期运动行被推迟，该行已被丢弃需重发，与其它 error 区分。
        PushResponse(code == 8 ? WaitResult::Deferred : WaitResult::Failed, code);
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
    // 逐行模式：发新行前清掉残留应答，防止上一行超时后迟到的 ok 满足本行的等待。
    // 窗口化模式必须关掉这个清空——那时队列里的应答是「在途」的合法凭据，
    // 清掉会让在途计数永久漂移（设计文档 §4.1）。
    if (drain_on_send_.load()) {
        DrainResponses();
    }

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

void Pipe::PushResponse(WaitResult result, int error_code) {
    // 队列满说明上游没有及时消费 —— 丢最老的，保留最新的。窗口化流控的在途计数
    // 由 Job 侧的 c_line 独立维护，丢应答只会让窗口偏保守（少发），不会误发。
    RespItem item{result, error_code};
    if (xQueueSend(resp_queue_, &item, 0) != pdTRUE) {
        RespItem dropped;
        if (xQueueReceive(resp_queue_, &dropped, 0) == pdTRUE) {
            ESP_LOGW(TAG, "应答队列满，丢弃最老应答 result=%d", (int)dropped.result);
        }
        xQueueSend(resp_queue_, &item, 0);
    }
}

void Pipe::DrainResponses() {
    RespItem item;
    while (xQueueReceive(resp_queue_, &item, 0) == pdTRUE) {
    }
}

WaitResult Pipe::TakeResponse(uint32_t timeout_ms, int* error_code) {
    if (resp_queue_ == nullptr) {
        return WaitResult::Timeout;
    }
    RespItem item{};
    if (xQueueReceive(resp_queue_, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "等待 Grbl 应答超时 (%ums)", (unsigned)timeout_ms);
        return WaitResult::Timeout;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // 还原成原来的行文本形态（`ok` / `error:NN`）。改队列后 last_response_ 不再是
        // 接收泵直接赋的原文，这里按码重建，保持 WaitResponse(response=...) 的旧语义。
        if (item.result == WaitResult::Ok) {
            last_response_ = "ok";
        } else if (item.error_code >= 0) {
            last_response_ = "error:" + std::to_string(item.error_code);
        } else {
            last_response_ = "error";
        }
        last_error_code_ = item.error_code;
    }
    if (error_code != nullptr) {
        *error_code = item.error_code;
    }
    return item.result;
}

WaitResult Pipe::WaitResponse(uint32_t timeout_ms, std::string* response, int* error_code) {
    WaitResult wr = TakeResponse(timeout_ms, error_code);
    if (response != nullptr) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        *response = last_response_;
    }
    return wr;
}

bool Pipe::WaitOk(uint32_t timeout_ms, std::string* response) {
    return WaitResponse(timeout_ms, response, nullptr) == WaitResult::Ok;
}

} // namespace hutuji
