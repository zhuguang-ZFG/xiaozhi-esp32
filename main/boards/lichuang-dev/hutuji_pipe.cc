#include "hutuji_pipe.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_netif.h>
#include <cJSON.h>
#include <nvs_flash.h>

#include "application.h"
#include "board.h"
#include "lwip/sockets.h"
#include "plotter_provision.h"

#define TAG "HutujiPipe"

#define HUTUJI_PIPE_PORT 23

#define HUTUJI_NVS_NS "hutuji_pipe"
#define HUTUJI_NVS_KEY_IP "grbl_ip"

namespace hutuji {

namespace {
// 深度与流控窗口共用纯核心常量，覆盖最短非空 payload 可形成的最大在途行数。
constexpr UBaseType_t kRespQueueDepth = static_cast<UBaseType_t>(kResponseQueueDepth);

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
// （machine/grbl/a.java:379-408），串口侧两级静默 5s + 约 1s（machine/b/b.java:20-28；
//  第一级发 `?` 后 o 不刷新，下一 tick 即判死，见 docs/kxnx/findings/21 §2.1）。
// 本机 kPollIntervalSec=3s，取 7 次 ≈ 21s，与 keepalive 的 19s 同量级互为补充。
constexpr int kSilentPollLimit = 7;

// 连接后等对端 Telnet banner（"\r\nGrbl <ver> ...\r\n"）的最长时间。
// Grbl_Esp32 在每个新 Telnet 连接上无条件发 report_init_message（TelnetServer.cpp:217，
// ENABLE_TELNET_WELCOME_MSG 已开）。这条 banner 是「对端确实是写字机」的唯一被动指纹，
// 用来堵「局域网里任意开着 :23 的主机（如路由器）被当成写字机缓存/顶替」的活锁。
// 缓存命中与子网扫描共用此校验。非写字机的 :23 主机每台最多付一次此超时。
constexpr int kVerifyTimeoutMs = 1500;
constexpr char kGrblBanner[] = "Grbl ";

// R10-PIPE-01：裸连接授权探测可重试失败的有界重试。残留 Hold 时 `$I` 撞
// idleOrAlarm 门回 error:8；一击置 Failed 且唯一清除点是连接重建的话，Hold 期
// `?` 有应答、silent-poll 不判死、keepalive 不触发——连接活着就永不 ready。
// 8 次 × 5s ≈ 40s，覆盖「Run 将尽 / 外部上位机片刻后解除 Hold」的瞬态；持续
// Hold 耗尽后仍 fail closed，绝不代替用户复位（0x18 已否决，与换纸保护冲突）。
constexpr int kAuthProbeMaxRetries = 8;
constexpr uint32_t kAuthProbeRetryDelayMs = 5000;

// R22-PIPE-02：`$I` 发出后等应答的无声拍数上限（recv 超时拍 = kPollIntervalSec）。
// 4 拍 ≈ 12s。裸 `$I` 在 Idle 下是毫秒级应答，12s 足够宽；取这么宽是因为超时后
// 的动作分两路：对端挂起 → 只播报并冻结（不重发，避免在 client_buffer 里堆积），
// 对端未挂起 → 认定 `$I` 真丢了并按 R10-PIPE-01 有界重试重探。
constexpr int kAuthProbeSilentTickLimit = 4;

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
}  // namespace

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

void Pipe::PipeTaskEntry(void* arg) { static_cast<Pipe*>(arg)->PipeTask(); }

void Pipe::PipeTask() {
    while (esp_netif_get_nr_of_ifs() == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "网络接口就绪，开始连接写字机");

    uint32_t backoff_ms = kBackoffInitMs;
    int miss_attempts = 0;  // 连续「真失败」次数（SlotBusy/WaitingIp 不计入）
    while (true) {
        if (!ConnectOnce()) {
            const uint32_t delay_ms = DiscoverRetryDelayMs(last_discover_miss_, backoff_ms);
            ESP_LOGW(TAG, "连接写字机失败，%ums 后重试", (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if (ShouldAdvanceDiscoverBackoff(last_discover_miss_)) {
                backoff_ms = NextDiscoverBackoffMs(++miss_attempts, backoff_ms, kBackoffMaxMs);
            }
            continue;
        }
        backoff_ms = kBackoffInitMs;
        miss_attempts = 0;
        paper_changing_.store(PaperChangingState::Unknown);
        ready_.store(false);
        authorized_.store(false);
        ResetSettingsFingerprintState();
        DrainResponses();
        if (task_session_active_.load()) {
            // 进行中的绘图刚经历断连。旧 planner 可能仍在运动，或 M30 换纸仍在
            // 阻塞；此时 M5/G1 授权探针都会污染状态。先交给 Job 查询 Changing。
            auth_probe_stage_ = AuthProbeStage::Idle;
            // 仍保持业务 ready=false；仅受限 reset 事务可使用已验真的活动任务 session。
            ESP_LOGW(TAG, "绘图会话重连：暂停自动授权探测，等待断连分流");
        } else {
            auth_probe_stage_ = AuthProbeStage::WaitingBuildInfoOk;
        }
        // 新连接另起重试额度（R10-PIPE-01）与无声超时计数（R22-PIPE-02）。
        auth_probe_retries_ = 0;
        auth_probe_silent_ticks_ = 0;
        auth_probe_suspend_notified_ = false;

        // 新连接重建时先关闭「对端正长阻塞」标记：当前仅凭 banner 不能判定是否换纸。
        // 若 Job 随后 `[ESP901]` 查到 Changing=On，会显式打开，避免换纸期 silent-poll
        // 误杀 TCP。
        expect_blocking_peer_.store(false);
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            // 撤销旧 token 与新 session 发布在线性化点内完成。
            abort_reset_token_.Cancel();
            connection_seq_.fetch_add(1);
            connected_.store(true);
            if (!task_session_active_.load()) {
                SendRawLocked("$I\n", 3);
            }
        }
        ESP_LOGI(TAG, "已连接写字机 Telnet（%s:%d）", resolved_ip_, HUTUJI_PIPE_PORT);
        // R21-F04：播报去内网 IP（IP 留上面 ESP_LOGI 日志）。
        NotifyCloud("写字机已连接");

        uint8_t buf[256];
        // 连续「探活已发但一个字节都没回」的次数。任何数据到达即归零。
        int silent_polls = 0;
        while (true) {
            int len = -1;
            uint32_t receive_epoch = 0;
            {
                // 全局锁序：reset_receive_mutex_ -> write_mutex_。完整行分派也在 receive
                // 排他区内，不能让已 recv 到本地 buf 的旧 banner 跨过 reset Arm 边界。
                std::lock_guard<std::mutex> receive_lock(reset_receive_mutex_);
                len = recv(sock_, buf, sizeof(buf), 0);
                if (len > 0) {
                    receive_epoch = reset_receive_epoch_.fetch_add(1U) + 1U;
                    silent_polls = 0;
                    OnRxData(buf, static_cast<size_t>(len), receive_epoch);
                }
            }
            if (len > 0) {
                continue;
            } else if (len == 0) {
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Changing=On 分流结束后，Job 会释放会话保护。由 Pipe 自己在
                    // 接收任务内恢复标准探测，避免跨任务并发修改 auth_probe_stage_。
                    // RetryWait 到期的重探也走这里（R10-PIPE-01）——同一驱动点，
                    // 保持 auth_probe_stage_ 单任务修改。
                    const GrblState gs = grbl_state_.load();
                    // R22-PIPE-02：挂起态下行命令不会被消费，此时发 `$I` 只会在对端
                    // client_buffer 里排队，解除后一次性回出多个 `ok` 与后续阶段错配。
                    // 因此挂起期一律不武装/不重探，等挂起解除再发。
                    const bool suspend_blocks_lines = GrblSuspendBlocksLines(
                        gs == GrblState::Hold, gs == GrblState::Door, gs == GrblState::Sleep);
                    if (!task_session_active_.load() && !ready_.load() && !suspend_blocks_lines &&
                        (auth_probe_stage_ == AuthProbeStage::Idle ||
                         (auth_probe_stage_ == AuthProbeStage::RetryWait &&
                          AuthProbeRetryDue(xTaskGetTickCount(), auth_probe_retry_due_tick_)))) {
                        auth_probe_stage_ = AuthProbeStage::WaitingBuildInfoOk;
                        auth_probe_silent_ticks_ = 0;
                        std::lock_guard<std::mutex> wlock(write_mutex_);
                        SendRawLocked("$I\n", 3);
                        silent_polls = 0;
                        continue;
                    }
                    // R22-PIPE-02：`$I` 已发但无声挂死的判定。挂起期 `?` 照常有应答，
                    // 所以 silent_polls 恒被状态行清零、keepalive 也不触发——不自己
                    // 数拍就永远停在 WaitingBuildInfoOk。
                    if (auth_probe_stage_ == AuthProbeStage::WaitingBuildInfoOk &&
                        !task_session_active_.load()) {
                        ++auth_probe_silent_ticks_;
                        switch (DecideAuthProbeStall(auth_probe_silent_ticks_,
                                                     kAuthProbeSilentTickLimit,
                                                     suspend_blocks_lines)) {
                            case AuthProbeStall::ParkSuspended:
                                // 冻结在上限，等挂起解除后由 ParseStatusReport 自愈重置。
                                auth_probe_silent_ticks_ = kAuthProbeSilentTickLimit;
                                if (!auth_probe_suspend_notified_) {
                                    auth_probe_suspend_notified_ = true;
                                    ESP_LOGW(TAG,
                                             "授权探测停等：对端处于 %s（挂起态不消费行命令），"
                                             "等待用户在写字机侧恢复",
                                             GrblStateName(gs));
                                    NotifyCloud("写字机停在暂停状态，请在写字机上恢复运行后重试");
                                }
                                break;
                            case AuthProbeStall::Reprobe:
                                // 未挂起却 12s 无应答：`$I` 真丢了（缓冲溢出/丢包），
                                // 按有界重试重探，耗尽仍 fail closed。
                                auth_probe_silent_ticks_ = 0;
                                ScheduleAuthProbeRetryOrFail("$I 探活", -1);
                                break;
                            case AuthProbeStall::KeepWaiting:
                                break;
                        }
                    }
                    // recv 超时：发 ? 轮询 Grbl 状态。
                    // TCP keepalive 只能发现「TCP 层死了」；若对端协议栈活着而 Grbl
                    // 主循环假死（不回 `?`），keepalive 不触发，这里必须自己判死，
                    // 否则本循环会永远转圈。参考奎享 grbl/a.java:379-408 的同类机制。
                    if (++silent_polls >= kSilentPollLimit) {
                        if (expect_blocking_peer_.load()) {
                            // 已知对端正长阻塞（如换纸）：不会回 `?` 属预期，不判死。
                            // 真死连由 TCP keepalive 兜底。
                            silent_polls = kSilentPollLimit;
                        } else {
                            ESP_LOGW(TAG, "连续 %d 次探活无应答（约 %ds），判定链路假死",
                                     silent_polls, silent_polls * kPollIntervalSec);
                            break;
                        }
                    }
                    std::lock_guard<std::mutex> wlock(write_mutex_);
                    SendRawLocked("?", 1);
                    continue;
                }
                ESP_LOGW(TAG, "recv 错误: errno=%d (%s)", errno, strerror(errno));
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            connected_.store(false);
            CloseSocketLocked();
        }
        paper_changing_.store(PaperChangingState::Unknown);
        expect_blocking_peer_.store(false);
        auth_probe_stage_ = AuthProbeStage::Idle;
        grbl_state_.store(GrblState::Unknown);
        grbl_substate_.store(-1);
        NotifyCloud("写字机连接断开");
        {
            std::lock_guard<std::mutex> receive_lock(reset_receive_mutex_);
            rx_buffer_.clear();
        }
        // 断连必须清空应答队列：上次连接遗留的 ok 若留到下次，会被当成本次命令的
        // 应答，窗口化流控下等于在途计数凭空少一格（最终死锁）。
        DrainResponses();
        ESP_LOGW(TAG, "写字机 Telnet 已断开，%ums 后重连", (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

Pipe::PeerCheck Pipe::VerifyGrblPeer(int sock, int timeout_ms) {
    // 只读、不发任何字节：banner 由对端 Telnet accept 路径主动送出
    // （report_init_message → TelnetServer.cpp:217，ENABLE_TELNET_WELCOME_MSG 已开），
    // 不需要我们先发 `$I`（那要等 Grbl 主循环，换纸期会阻塞）。
    // banner 字节在此被消费掉、不再送达主循环 —— 但主循环连上后会主动发 `$I`，
    // 靠 `[VER:` 应答（ProcessLine :391）置 ready_，不依赖这条 banner。
    // banner 尾部残段（如 " for help]"）落入主循环也不匹配任何分支，无害。
    std::string probe;
    int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;

    while (esp_timer_get_time() < deadline_us) {
        int64_t left_us = deadline_us - esp_timer_get_time();
        struct timeval tv = {.tv_sec = static_cast<time_t>(left_us / 1000000),
                             .tv_usec = static_cast<suseconds_t>(left_us % 1000000)};
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sock, &rset);
        if (select(sock + 1, &rset, nullptr, nullptr, &tv) <= 0) {
            break;  // 超时或出错：连接仍开着但拿不到 banner
        }

        uint8_t buf[128];
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n == 0) {
            // Grbl MAX_TLNT_CLIENTS=1。旧半开连接占槽时，server 会 accept 后立即
            // close 新连接；不能把它误判成「缓存 IP 被别的设备顶替」。
            return probe.empty() ? PeerCheck::ClosedBeforeBanner : PeerCheck::Invalid;
        }
        if (n < 0) {
            break;
        }
        probe.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
        if (probe.find(kGrblBanner) != std::string::npos) {
            return PeerCheck::Valid;
        }
        if (probe.size() > kRxLineMax) {
            break;  // 一直在说话但不是 Grbl（例如某些设备的登录提示）
        }
    }
    return PeerCheck::Invalid;
}

bool Pipe::TryConnect(uint32_t ip_addr, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
        return false;

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
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
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
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    inet_ntoa_r(dest.sin_addr, resolved_ip_, sizeof(resolved_ip_));
    sock_ = s;
    return true;
}

bool Pipe::ConnectOnce() {
    // 配网跳窗（StopStation→跳连出厂 AP→回切）期间歇工：此时 WIFI_STA_DEF 指向
    // 跳配 netif（192.168.0.x），若照常拨号会连上写字机 AP 模式的 :23、把
    // 192.168.0.1 写进 NVS 缓存，还会对机器发授权运动探针。等回切后再发现。
    if (PlotterProvision::GetInstance().IsBusy()) {
        last_discover_miss_ = DiscoverMiss::WaitingIp;
        return false;
    }
    // 发现期钉 PERFORMANCE：LOW_POWER(MAX_MODEM) 下首包可到 ~200ms，50ms 扫网会漏检。
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    // 等 DHCP 拿到 IP 再尝试（避免 WiFi 连接前的无效重试）
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        last_discover_miss_ = DiscoverMiss::WaitingIp;
        return false;
    }

    bool found = false;
    bool cached_suspect = false;  // 缓存 IP 连上了但验证不是写字机（可能被 DHCP 改号顶替）
    uint32_t cached_ip = LoadCachedIp();
    last_discover_miss_ = cached_ip == 0 ? DiscoverMiss::ScanEmpty : DiscoverMiss::CacheUnreachable;

    // 0) NVS 缓存（上次发现的写字机，2s 超时容忍短暂不可达）
    if (cached_ip != 0) {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        if (TryConnect(cached_ip, kPipeCachedTimeoutMs)) {
            // 缓存命中也要验明正身：若 DHCP 把该 IP 分给了别的设备（可能开 :23），
            // 不能把它当写字机连上。真正无效才清缓存；槽位忙保留并重试。
            PeerCheck check = VerifyGrblPeer(sock_, kVerifyTimeoutMs);
            if (check == PeerCheck::Valid) {
                ESP_LOGI(TAG, "缓存 IP 命中: %s", resolved_ip_);
                cached_slot_busy_count_ = 0;
                found = true;
            } else if (check == PeerCheck::ClosedBeforeBanner) {
                last_discover_miss_ = DiscoverMiss::SlotBusy;
                ++cached_slot_busy_count_;
                ESP_LOGW(TAG, "缓存 IP %s 的 Telnet 槽位忙（第 %u 次），1s 后重试缓存",
                         resolved_ip_, static_cast<unsigned>(cached_slot_busy_count_));
                close(sock_);
                sock_ = -1;
            } else {
                ESP_LOGW(TAG, "缓存 IP %s 验证失败（非写字机），清除缓存并回落扫描", resolved_ip_);
                last_discover_miss_ = DiscoverMiss::CacheNotGrbl;
                cached_suspect = true;
                cached_slot_busy_count_ = 0;
                close(sock_);
                sock_ = -1;
            }
        } else {
            last_discover_miss_ = DiscoverMiss::CacheUnreachable;
        }
    }

    // 槽位忙：只重试缓存。扫网会跳过/漏掉真机，把 Grbl keepalive ~19s 放大成分钟级。
    if (!found && ShouldScanSubnet(last_discover_miss_)) {
        uint32_t base = ip_info.ip.addr & ip_info.netmask.addr;
        uint32_t self = ip_info.ip.addr;
        ESP_LOGI(TAG, "扫描子网寻找写字机 Telnet:23...");
        std::lock_guard<std::mutex> lock(sock_mutex_);
        for (int host = 1; host < 255; ++host) {
            uint32_t target = base | htonl(host);
            if (target == self)
                continue;
            if (target == cached_ip && SkipCachedIpDuringScan(last_discover_miss_))
                continue;
            if (TryConnect(target, kPipeScanTimeoutMs)) {
                // 只认真写字机：banner 校验不过就关掉继续扫，
                // 否则路由器/打印机等开 :23 的主机会被当成写字机顶替（A）。
                if (VerifyGrblPeer(sock_, kVerifyTimeoutMs) == PeerCheck::Valid) {
                    ESP_LOGI(TAG, "子网扫描发现写字机 %s", resolved_ip_);
                    cached_slot_busy_count_ = 0;
                    found = true;
                    break;
                }
                close(sock_);
                sock_ = -1;
            }
        }
        if (!found && cached_ip != 0 && RetryCachedIpAfterScan(last_discover_miss_, found)) {
            if (TryConnect(cached_ip, kPipeCachedTimeoutMs) &&
                VerifyGrblPeer(sock_, kVerifyTimeoutMs) == PeerCheck::Valid) {
                ESP_LOGI(TAG, "扫网后缓存 IP 命中: %s", resolved_ip_);
                cached_slot_busy_count_ = 0;
                found = true;
            } else if (sock_ >= 0) {
                close(sock_);
                sock_ = -1;
            }
        }
    }

    if (!found) {
        if (cached_suspect) {
            // 本轮没找回写字机，把失真的缓存清掉，避免下轮再被它带偏。
            ESP_LOGW(TAG, "清除失真缓存 IP，下轮重扫");
            nvs_handle_t h;
            if (nvs_open(HUTUJI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, HUTUJI_NVS_KEY_IP);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        if (last_discover_miss_ != DiscoverMiss::SlotBusy &&
            last_discover_miss_ != DiscoverMiss::WaitingIp &&
            last_discover_miss_ != DiscoverMiss::CacheNotGrbl &&
            last_discover_miss_ != DiscoverMiss::CacheUnreachable) {
            last_discover_miss_ = DiscoverMiss::ScanEmpty;
        }
        return false;
    }

    // 缓存 IP 到 NVS（下次启动秒连）。到这里两条路径都已通过 VerifyGrblPeer，
    // 只缓存确认是写字机的 IP（A）。IP 未变时不写；变了（含缓存失真后扫到新址、
    // 或写字机被 DHCP 改号）才写。
    struct sockaddr_in peer = {};
    socklen_t plen = sizeof(peer);
    if (getpeername(sock_, reinterpret_cast<struct sockaddr*>(&peer), &plen) == 0 &&
        peer.sin_addr.s_addr != cached_ip) {
        SaveCachedIp(peer.sin_addr.s_addr);
        ESP_LOGI(TAG, "写字机 IP 已缓存到 NVS");
    }

    // 每条 G-code 都是小包；关闭 Nagle，避免 Z5 已执行并在 `$1=25ms` 后失能，
    // 下一条 XY 仍因等待前包 ACK 滞留，造成“轨迹走但笔已回弹”。
    int no_delay = 1;
    if (setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) {
        ESP_LOGE(TAG, "设置 TCP_NODELAY 失败: errno=%d (%s)", errno, strerror(errno));
        CloseSocket();
        return false;
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
    struct timeval recv_tv = {.tv_sec = kPollIntervalSec, .tv_usec = 0};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    return true;
}

void Pipe::CloseSocket() {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    CloseSocketLocked();
}

void Pipe::CloseSocketLocked() {
    abort_reset_token_.Cancel();
    ready_.store(false);
    authorized_.store(false);
    std::lock_guard<std::mutex> sock_lock(sock_mutex_);
    if (sock_ >= 0) {
        // FluidNC #189 / ESP3D：客户端须 stop/shutdown，服务端才能放掉唯一槽位。
        shutdown(sock_, SHUT_RDWR);
        close(sock_);
        sock_ = -1;
    }
}

void Pipe::ShutdownSocket(uint32_t expected_connection_sequence) {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    if (connection_seq_.load() != expected_connection_sequence) {
        return;
    }
    abort_reset_token_.Cancel();
    ready_.store(false);
    authorized_.store(false);
    std::lock_guard<std::mutex> sock_lock(sock_mutex_);
    if (sock_ >= 0) {
        shutdown(sock_, SHUT_RDWR);
    }
}

void Pipe::CancelAbortReset() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    abort_reset_token_.Cancel();
}

bool Pipe::SendRawLocked(const char* data, size_t len, bool feed_hold_priority) {
    size_t sent = 0;
    int send_errno = 0;
    int last_result = -1;
    bool budget_expired = false;
    const uint32_t send_began = static_cast<uint32_t>(xTaskGetTickCount());
    SendStallBudget budget(send_began, static_cast<uint32_t>(pdMS_TO_TICKS(kSendStallBudgetMs)));

    while (sent < len) {
        const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount());
        if (budget.Expired(now)) {
            budget_expired = true;
            break;
        }

        // 普通发送不占着 socket 等背压；有 feed hold 等待时先让实时字符取得发送机会。
        // 仲裁等待不是 TCP 停滞，不消耗普通发送的背压预算。
        if (ShouldYieldToFeedHold(feed_hold_priority,
                                  feed_hold_waiters_.load(std::memory_order_acquire))) {
            const uint32_t suspended_from = now;
            do {
                vTaskDelay(1);
            } while (ShouldYieldToFeedHold(feed_hold_priority,
                                           feed_hold_waiters_.load(std::memory_order_acquire)));
            budget.Suspend(suspended_from, static_cast<uint32_t>(xTaskGetTickCount()));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(sock_mutex_);
            if (sock_ < 0) {
                return false;
            }
            last_result = send(sock_, data + sent, len - sent, MSG_DONTWAIT);
            send_errno = last_result < 0 ? errno : 0;  // 紧贴失败 send() 捕获，防后续调用改写
        }
        if (AdvanceSendProgress(sent, last_result)) {
            continue;
        }
        if (ShouldRetrySend(last_result, send_errno)) {
            vTaskDelay(1);
            continue;
        }
        break;
    }
    if (sent == len) {
        return true;
    }

    if (budget_expired) {
        ESP_LOGE(TAG, "send 活动时间超过 %lu ms，判定链路死亡，强制重建当前连接",
                 (unsigned long)kSendStallBudgetMs);
    } else {
        ESP_LOGE(TAG, "send 失败: n=%d errno=%d (%s)，强制重建当前连接", last_result, send_errno,
                 strerror(send_errno));
    }
    // feed hold 快路径不持 write_mutex_：绝不在此裸改 abort_reset_token_/ready_/authorized_
    // 或半关 socket——那会破坏 AbortResetToken 的单写者锁串行化契约。整套 teardown 改由
    // 调用方 SendFeedHold 在失败后经 write_mutex_ 保护的 ShutdownSocket() 串行化完成。
    if (feed_hold_priority) {
        return false;
    }
    // 普通/reset 路径：调用方已持 write_mutex_，就地完成 teardown。
    // 之前的成功迭代可能已写出半条命令；半关连接唤醒 recv 泵并强制重连，
    // 绝不让下一条命令与对端残留半行拼接。
    abort_reset_token_.Cancel();
    ready_.store(false);
    authorized_.store(false);
    std::lock_guard<std::mutex> lock(sock_mutex_);
    if (sock_ >= 0) {
        shutdown(sock_, SHUT_RDWR);
    }
    return false;
}

void Pipe::OnRxData(const uint8_t* data, size_t data_len, uint32_t receive_epoch) {
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
            ProcessLine(line, receive_epoch);
        }
    }
    if (rx_buffer_.size() > kRxLineMax) {
        ESP_LOGW(TAG, "接收残段超长，丢弃 %zu 字节", rx_buffer_.size());
        rx_buffer_.clear();
    }
}

int Pipe::ParseErrorCode(const std::string& line) {
    // 实现在 hutuji_recovery_core.h：纯函数便于 host 编译单测覆盖数字与
    // `$Errors/Verbose=1` 文本两种形态（见该处注释说明为何必须认文本）。
    return ParseGrblErrorCode(line);
}

void Pipe::ProcessLine(const std::string& line, uint32_t receive_epoch) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_line_ = line;
    }
    // 窗口化灌流的 ok 每行一条，UART 115200 阻塞打印 ~2-3ms/条且占 PipeTask 收泵
    // 时间；裸 ok 无诊断信息量（进度由 job 侧节流计数兜底），仅窗口化模式压制，
    // 状态报告/错误/ALARM 与逐行模式（归位、换纸、探测）仍全量记录。
    if (!HUTUJI_QUIET_STREAM_LOG || drain_on_send_.load() || line != "ok") {
        ESP_LOGI(TAG, "<- %s", line.c_str());
    }

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
            // R21-F04：报警码保留（售后定位）；机器坐标对用户无意义（串口逐行日志仍有）。
            char buf[64];
            std::snprintf(buf, sizeof(buf), "写字机报警，代码 %d", code);
            NotifyCloud(buf);
        }
        return;
    }

    if (line.find("[License] Authorized") != std::string::npos) {
        authorized_.store(true);
    }

    if (line.find("Paper=") != std::string::npos && line.find("Changing=") != std::string::npos) {
        // P1-1：同一份应答顺带解析三字段（与 Changing 同一守卫，缺席保持 Unknown）。
        PaperPresentState paper = PaperPresentState::Unknown;
        MotorEnState motor = MotorEnState::Unknown;
        PanelHoldState panel = PanelHoldState::Unknown;
        ParsePaperStatusFields(line, paper, motor, panel);
        paper_present_.store(paper);
        motor_en_.store(motor);
        panel_hold_.store(panel);
        if (line.find("Changing=On") != std::string::npos) {
            paper_changing_.store(PaperChangingState::On);
        } else if (line.find("Changing=Off") != std::string::npos) {
            paper_changing_.store(PaperChangingState::Off);
        } else {
            paper_changing_.store(PaperChangingState::Unknown);
        }
        paper_status_seq_.fetch_add(1);
        return;
    }

    if (line.rfind("[VER:", 0) == 0) {
        // `$I` 只证明对端版本；商业固件的授权日志走 CLIENT_SERIAL，Telnet 看不到。
        // ready_ 要等后续零位移授权探测完成，避免 draw 在授权态未知时抢跑。
        ESP_LOGI(TAG, "Grbl 版本探活成功（%s），等待授权探测", line.c_str());
        return;
    }

    if (line.rfind("Grbl ", 0) == 0) {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        const uint32_t banner_generation = reset_banner_seq_.fetch_add(1) + 1U;
        ready_.store(false);
        authorized_.store(false);
        ResetSettingsFingerprintState();
        const bool recover_abort =
            abort_reset_token_.Consume(connection_seq_.load(), banner_generation, receive_epoch);
        if (recover_abort) {
            auth_probe_stage_ = AuthProbeStage::WaitingAbortUnlockOk;
        } else if (task_session_active_.load()) {
            auth_probe_stage_ = AuthProbeStage::Idle;
        } else {
            auth_probe_stage_ = AuthProbeStage::WaitingBuildInfoOk;
        }
        // 对端已复位 = 全新机器状态，重试额度另起（R10-PIPE-01）；复位也清掉挂起
        // 期的无声计数与播报闸（R22-PIPE-02）——复位后 client_buffer 已空，排队的
        // `$I` 不复存在，上面刚重发的那条是唯一在途探针。
        auth_probe_retries_ = 0;
        auth_probe_silent_ticks_ = 0;
        auth_probe_suspend_notified_ = false;
        // 对端软复位会丢弃其内部排队状态，我方遗留应答全部失效。
        DrainResponses();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_.clear();
            last_error_code_ = -1;
        }
        if (recover_abort) {
            // 仅处理本机刚发出的 abort reset 所产生的锁定；其它 Alarm 不自动解锁。
            SendRawLocked("$X\n", 3);
        } else if (!task_session_active_.load()) {
            SendRawLocked("$I\n", 3);
        } else {
            // S3-P3e 已评估：外部 reset（banner 到达但 TCP 未断）没有同连接恢复
            // 路径——RecoverDisconnectedDraw 只认连接 seq 变化，本场景任务会在
            // 等 ok/等 Idle 处按超时失败（≤ 对应预算）。维持 fail-closed：对端
            // 复位已丢弃 planner 与模态，续画必然错位；且外部 0x18 意味着有别的
            // 上位机在操作机器，S3 不应与之抢状态。代价是有界的等待，不加恢复。
            ESP_LOGW(TAG, "绘图会话内检测到 reset banner，等待任务层断连分流");
        }
        return;
    }

    if (auth_probe_stage_ == AuthProbeStage::WaitingSettingQuery && !line.empty() &&
        line[0] == '$') {
        std::string key;
        double value = 0.0;
        if (!ParseGrblSettingLine(line, key, value)) {
            settings_line_ok_ = false;
            RecordSettingsMismatch("parse");
        } else {
            const GrblSettingCheckResult check =
                CheckGrblSettingAgainstGolden(static_cast<size_t>(settings_query_index_), key, value);
            settings_line_ok_ = check.ok;
            if (!check.ok) {
                RecordSettingsMismatch(check.key);
            }
        }
        return;
    }

    // 只有 `ok` 与 `error:NN` 入应答队列。banner / `[VER:` / `<...>` 状态行
    // 都在上面提前 return —— 对应官方 stream.py 的「非应答行不消耗窗口」（R3）。
    if (line == "ok") {
        if (HandleAuthProbeResponse(WaitResult::Ok, -1)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = line;
            last_error_code_ = -1;
        }
        PushResponse(WaitResult::Ok, -1);
    } else if (line.rfind("error", 0) == 0) {
        int code = ParseErrorCode(line);
        if (HandleAuthProbeResponse(code == 8 ? WaitResult::Deferred : WaitResult::Failed, code)) {
            return;
        }
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
    std::string state_str = (pipe_pos != std::string::npos) ? content.substr(0, pipe_pos) : content;

    // Hold:0 / Alarm:1 等带子码
    int sub_code = 0;
    size_t colon = state_str.find(':');
    if (colon != std::string::npos) {
        sub_code = std::atoi(state_str.c_str() + colon + 1);
        state_str = state_str.substr(0, colon);
    }

    GrblState gs = GrblState::Unknown;
    if (state_str == "Idle")
        gs = GrblState::Idle;
    else if (state_str == "Run")
        gs = GrblState::Run;
    else if (state_str == "Hold")
        gs = GrblState::Hold;
    else if (state_str == "Jog")
        gs = GrblState::Jog;
    else if (state_str == "Alarm")
        gs = GrblState::Alarm;
    else if (state_str == "Door")
        gs = GrblState::Door;
    else if (state_str == "Check")
        gs = GrblState::Check;
    else if (state_str == "Home")
        gs = GrblState::Home;
    else if (state_str == "Sleep")
        gs = GrblState::Sleep;

    grbl_substate_.store(colon != std::string::npos ? sub_code : -1);
    GrblState prev = grbl_state_.exchange(gs);
    if (prev != gs) {
        ESP_LOGI(TAG, "Grbl 状态: %s -> %s", GrblStateName(prev), GrblStateName(gs));
        // R21-F04：播报去英文状态名/机器坐标（用户无意义）；Run→Idle 与 Hold 加
        // 10s 去抖——页尾归位会确定性连触发两次 Run→Idle（最后一行完成一次、
        // 归位 G1 完成一次）。坐标仍在 :601 的逐行串口日志。
        const TickType_t now = xTaskGetTickCount();
        if (gs == GrblState::Alarm) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "写字机报警，代码 %d", sub_code);
            NotifyCloud(buf);
        } else if (prev == GrblState::Run && gs == GrblState::Idle) {
            if (!transition_notify_suppressed_.load() &&
                now - last_transition_notify_tick_ >= pdMS_TO_TICKS(10000)) {
                last_transition_notify_tick_ = now;
                NotifyCloud("写字机运动完成");
            }
        } else if (gs == GrblState::Hold) {
            if (!transition_notify_suppressed_.load() &&
                now - last_transition_notify_tick_ >= pdMS_TO_TICKS(10000)) {
                last_transition_notify_tick_ = now;
                NotifyCloud("写字机已暂停");
            }
        }

        // R22-PIPE-02：挂起解除的自愈点。挂起期排队在对端 client_buffer 里的 `$I`
        // 会在 `protocol_poll_client()` 恢复调用后被消费并回应答，所以这里只清零
        // 无声计数（给它一个完整的判定窗口）与播报闸，绝不重发——重发会多出一个
        // `ok`，与后续 M5/G53 阶段的应答错配。若排队的 `$I` 真丢了，下一轮无声
        // 超时会走 Reprobe 分支按有界重试重发。
        if (ShouldRearmStalledProbe(
                auth_probe_stage_ == AuthProbeStage::WaitingBuildInfoOk,
                GrblSuspendBlocksLines(prev == GrblState::Hold, prev == GrblState::Door,
                                       prev == GrblState::Sleep),
                GrblSuspendBlocksLines(gs == GrblState::Hold, gs == GrblState::Door,
                                       gs == GrblState::Sleep))) {
            auth_probe_silent_ticks_ = 0;
            auth_probe_suspend_notified_ = false;
            ESP_LOGI(TAG, "挂起解除（%s -> %s），授权探测恢复等待 $I 应答", GrblStateName(prev),
                     GrblStateName(gs));
        }
    }

    // MPos：只把完整有限三轴作为新位置证据。$10 可持久化为 WPos；网络/固件异常
    // 也可能产生 NaN/Inf。两者都可推进通用状态序号，但不得推进位置序号。
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (ParseFiniteMPos(content, x, y, z)) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            mpos_x_ = x;
            mpos_y_ = y;
            mpos_z_ = z;
        }
        mpos_report_seq_.fetch_add(1);
        if (auth_probe_stage_ == AuthProbeStage::WaitingAbortLiftIdle && gs == GrblState::Idle) {
            // Z 抬笔行的 ok 只表示进入 planner；实机上紧接着发 $I 会在 Run 态
            // 收到 error:8。等新状态确认 Idle 后再继续探活。
            auth_probe_stage_ = AuthProbeStage::WaitingBuildInfoOk;
            auth_probe_silent_ticks_ = 0;
            if (!SendLine("$I")) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "abort 复位后探活发送失败");
            }
        } else if (auth_probe_stage_ == AuthProbeStage::WaitingPosition && gs == GrblState::Idle) {
            // 授权状态没有 Telnet 查询命令。先 M5 抬笔，再以机器坐标发送当前 X 的
            // G1 零位移行：已授权回 ok，未授权由 Protocol.cpp 的行首 G0~G3
            // 前置门回 error:110。
            auth_probe_stage_ = AuthProbeStage::WaitingMotionReply;
            if (!SendLine(BuildLicenseProbeLine(x))) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "授权探测发送失败");
            }
        }
    }

    if (gs == GrblState::Alarm) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        alarm_code_ = sub_code;
    }
    // 最后递增：等待方看到新序号时，上面的状态与坐标已经全部更新。
    status_report_seq_.fetch_add(1);
}

bool Pipe::HandleAuthProbeResponse(WaitResult result, int error_code) {
    switch (auth_probe_stage_) {
        case AuthProbeStage::WaitingAbortUnlockOk:
            if (result != WaitResult::Ok) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "abort 复位后解锁失败: error:%d", error_code);
                return true;
            }
            // soft reset 已把主轴模态重置为 Off；此时单发 M5 会被当作无状态变化，
            // 实机验证会留下 Z=5。用明确 Z 运动先抬笔，避免 M3->M5 在本来已抬笔时落点。
            auth_probe_stage_ = AuthProbeStage::WaitingAbortLiftOk;
            if (!SendLine("G1G90 Z0.0F10000")) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "abort 复位后抬笔发送失败");
            }
            return true;

        case AuthProbeStage::WaitingAbortLiftOk:
            if (result != WaitResult::Ok) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "abort 复位后抬笔失败: error:%d", error_code);
                return true;
            }
            auth_probe_stage_ = AuthProbeStage::WaitingAbortLiftIdle;
            if (!SendRealtime('?')) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "abort 复位后抬笔状态查询失败");
            }
            return true;

        case AuthProbeStage::WaitingBuildInfoOk:
            if (result != WaitResult::Ok) {
                // 典型是残留 Hold 撞 idleOrAlarm 门的 error:8（Deferred）：机器
                // 状态可能片刻后变化，走有界重试而不是一击进 Failed 终态。
                ScheduleAuthProbeRetryOrFail("$I 探活", error_code);
                return true;
            }
            // 探测运动前先确定抬笔；M 指令不受授权门限制。
            auth_probe_stage_ = AuthProbeStage::WaitingLiftOk;
            if (!SendLine("M5")) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "授权探测抬笔发送失败");
            }
            return true;

        case AuthProbeStage::WaitingLiftOk:
            if (result != WaitResult::Ok) {
                ScheduleAuthProbeRetryOrFail("授权探测抬笔", error_code);
                return true;
            }
            auth_probe_stage_ = AuthProbeStage::WaitingPosition;
            if (!SendRealtime('?')) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "授权探测位置查询失败");
            }
            return true;

        case AuthProbeStage::WaitingMotionReply:
            if (result == WaitResult::Ok) {
                authorized_.store(true);
                ESP_LOGI(TAG, "授权探测通过（抬笔后 G53 零位移）");
                if (!BeginSettingsFingerprintProbe()) {
                    auth_probe_stage_ = AuthProbeStage::Failed;
                }
            } else if (result == WaitResult::Failed && error_code == 110) {
                authorized_.store(false);
                ESP_LOGW(TAG, "写字机未授权（零位移探测返回 error:110）");
                if (!BeginSettingsFingerprintProbe()) {
                    auth_probe_stage_ = AuthProbeStage::Failed;
                }
            } else {
                // error:110 之外的失败不是授权结论（如换纸窗口的 error:8）：
                // 保持 ready=false 并有界重试，别把瞬态当权威。
                authorized_.store(false);
                ready_.store(false);
                ScheduleAuthProbeRetryOrFail("零位移授权探测", error_code);
            }
            return true;

        case AuthProbeStage::WaitingSettingQuery:
            if (result != WaitResult::Ok) {
                ScheduleAuthProbeRetryOrFail("设置指纹", error_code);
                return true;
            }
            if (!settings_line_ok_) {
                ready_.store(true);
                auth_probe_stage_ = AuthProbeStage::Complete;
                ESP_LOGE(TAG, "Grbl 设置指纹不符（$%s）", GetSettingsMismatchKey().c_str());
                NotifyCloud("写字机参数被改动，请联系卖家恢复后再画");
                return true;
            }
            ++settings_query_index_;
            if (static_cast<size_t>(settings_query_index_) >= kGrblSettingGoldenCount) {
                settings_verified_.store(true);
                ready_.store(true);
                auth_probe_stage_ = AuthProbeStage::Complete;
                ESP_LOGI(TAG, "Grbl 设置指纹通过（%u 项）",
                         static_cast<unsigned>(kGrblSettingGoldenCount));
                return true;
            }
            settings_line_ok_ = false;
            if (!SendLine(kGrblSettingGoldens[settings_query_index_].query_line)) {
                auth_probe_stage_ = AuthProbeStage::Failed;
                ESP_LOGE(TAG, "设置指纹查询发送失败");
            }
            return true;

        default:
            return false;
    }
}

void Pipe::ScheduleAuthProbeRetryOrFail(const char* what, int error_code) {
    // error_code < 0 = 无 error 行的失败（R22-PIPE-02 的无声超时）；此时打
    // "error:-1" 会误导排障，单独成文案。
    char reason[24];
    if (error_code < 0) {
        std::snprintf(reason, sizeof(reason), "超时无应答");
    } else {
        std::snprintf(reason, sizeof(reason), "error:%d", error_code);
    }
    if (DecideAuthProbeFailure(auth_probe_retries_, kAuthProbeMaxRetries) ==
        AuthProbeFailure::RetryLater) {
        ++auth_probe_retries_;
        auth_probe_stage_ = AuthProbeStage::RetryWait;
        auth_probe_retry_due_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(kAuthProbeRetryDelayMs);
        ESP_LOGW(TAG, "%s失败: %s，%ums 后重探（第 %d/%d 次）", what, reason,
                 (unsigned)kAuthProbeRetryDelayMs, auth_probe_retries_, kAuthProbeMaxRetries);
        return;
    }
    auth_probe_stage_ = AuthProbeStage::Failed;
    ESP_LOGE(TAG, "%s失败: %s，重试额度已耗尽，等待连接重建", what, reason);
    // 用户可感知的死角：连接活着但永不 ready。给云端一条可读解释。
    NotifyCloud("写字机授权探测多次失败（可能停在暂停/保持状态），请检查写字机后重试");
}

void Pipe::ResetSettingsFingerprintState() {
    settings_verified_.store(false);
    settings_query_index_ = 0;
    settings_line_ok_ = false;
    {
        std::lock_guard<std::mutex> lock(settings_mismatch_mutex_);
        settings_mismatch_key_.clear();
    }
}

void Pipe::RecordSettingsMismatch(const std::string& key) {
    std::lock_guard<std::mutex> lock(settings_mismatch_mutex_);
    if (settings_mismatch_key_.empty()) {
        settings_mismatch_key_ = key;
    }
}

bool Pipe::BeginSettingsFingerprintProbe() {
    ResetSettingsFingerprintState();
    auth_probe_stage_ = AuthProbeStage::WaitingSettingQuery;
    settings_line_ok_ = false;
    if (!SendLine(kGrblSettingGoldens[0].query_line)) {
        ESP_LOGE(TAG, "设置指纹首项发送失败");
        return false;
    }
    return true;
}

std::string Pipe::GetSettingsMismatchKey() const {
    std::lock_guard<std::mutex> lock(settings_mismatch_mutex_);
    return settings_mismatch_key_;
}

const char* Pipe::GrblStateName(GrblState s) {
    switch (s) {
        case GrblState::Unknown:
            return "Unknown";
        case GrblState::Idle:
            return "Idle";
        case GrblState::Run:
            return "Run";
        case GrblState::Hold:
            return "Hold";
        case GrblState::Jog:
            return "Jog";
        case GrblState::Alarm:
            return "Alarm";
        case GrblState::Door:
            return "Door";
        case GrblState::Check:
            return "Check";
        case GrblState::Home:
            return "Home";
        case GrblState::Sleep:
            return "Sleep";
        default:
            return "?";
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
    // 窗口化灌流的运动行每行 ~4-8ms 阻塞打印（UART 115200，持 write_mutex_），
    // 是 S3 侧逐行固定成本大头；仅压制 G0-G3 运动行，控制行与逐行模式全量保留。
    if (!HUTUJI_QUIET_STREAM_LOG || drain_on_send_.load() || !IsStreamingMotionLine(line)) {
        ESP_LOGI(TAG, "-> %s", line.c_str());
    }
    return true;
}

bool Pipe::HasFreshStoppedStatus(uint32_t previous_status_sequence,
                                 uint32_t expected_connection_sequence) const {
    if (connection_seq_.load() != expected_connection_sequence ||
        status_report_seq_.load() == previous_status_sequence) {
        return false;
    }
    const GrblState state = grbl_state_.load();
    return IsStoppedForReset(state == GrblState::Idle, state == GrblState::Hold,
                             grbl_substate_.load() == 0);
}

bool Pipe::SendAbortReset(uint32_t expected_connection_sequence, uint32_t previous_status_sequence,
                          uint32_t previous_paper_status_sequence,
                          uint32_t previous_banner_sequence, bool allow_unready_reconnect) {
    // 先挡住 recv 泵并处理 socket 中已经到达的旧字节；只有这些字节完成分派后，
    // 才能定义本次 0x18 的接收 epoch 边界。
    std::unique_lock<std::mutex> receive_lock(reset_receive_mutex_);
    std::string pending_rx;
    uint32_t pending_epoch = reset_receive_epoch_.load();
    {
        std::lock_guard<std::mutex> sock_lock(sock_mutex_);
        uint8_t pending[256];
        while (sock_ >= 0) {
            int n = recv(sock_, pending, sizeof(pending), MSG_DONTWAIT);
            if (n <= 0) {
                break;
            }
            pending_rx.append(reinterpret_cast<const char*>(pending), static_cast<size_t>(n));
            pending_epoch = reset_receive_epoch_.fetch_add(1U) + 1U;
        }
    }
    if (!pending_rx.empty()) {
        OnRxData(reinterpret_cast<const uint8_t*>(pending_rx.data()), pending_rx.size(),
                 pending_epoch);
    }
    // 旧残段跨越 0x18 边界时无法证明后续完整 banner 属于本次 reset，直接拒绝。
    if (!rx_buffer_.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(write_mutex_);
    const bool same_session = connection_seq_.load() == expected_connection_sequence;
    const bool reset_session_ready =
        IsResetSessionReady(expected_connection_sequence, allow_unready_reconnect);
    const bool fresh_stopped =
        HasFreshStoppedStatus(previous_status_sequence, expected_connection_sequence);
    const bool fresh_paper = paper_status_seq_.load() != previous_paper_status_sequence;
    const bool banner_unchanged = reset_banner_seq_.load() == previous_banner_sequence;
    if (!same_session || !banner_unchanged) {
        abort_reset_token_.Cancel();
        return false;
    }
    if (!CanSendAbortReset(connected_.load(), reset_session_ready, true, fresh_stopped, fresh_paper,
                           paper_changing_.load() == PaperChangingState::Off)) {
        return false;
    }

    // recv 泵仍被挡住：先绑定当前 epoch/下一代 banner，再完整写出 0x18；解锁后
    // 第一个新 recv 必然获得严格晚于基线的 epoch，旧排队 banner 无法 Consume。
    const uint32_t receive_baseline = reset_receive_epoch_.load();
    if (!abort_reset_token_.Arm(expected_connection_sequence, previous_banner_sequence,
                                receive_baseline)) {
        return false;
    }
    ready_.store(false);
    authorized_.store(false);
    const char reset = static_cast<char>(0x18);
    if (!SendRawLocked(&reset, 1)) {
        abort_reset_token_.Cancel();
        return false;
    }
    return true;
}

bool Pipe::SendFeedHold() {
    // 快路径整段不占 write_mutex_，保证 `!` 能在普通发送重试间隙抢占。先捕获当前 session：
    // 失败 teardown 用它做守卫，连接已轮换时 ShutdownSocket 自会跳过，不误伤新 session。
    const uint32_t session = connection_seq_.load();
    feed_hold_waiters_.fetch_add(1, std::memory_order_acq_rel);
    const char hold = '!';
    const bool sent = SendRawLocked(&hold, 1, true);
    feed_hold_waiters_.fetch_sub(1, std::memory_order_acq_rel);
    if (!sent) {
        // SendRawLocked 在 feed_hold_priority 下只记日志、不碰共享状态。整套 teardown
        // （Cancel token / 清 ready/authorized / 半关 socket）改由此处经 write_mutex_ 串行化，
        // 恢复 AbortResetToken 的单写者锁契约（recovery_core.h Arm/Consume/Cancel 同锁约束）。
        ShutdownSocket(session);
    }
    return sent;
}

bool Pipe::SendRealtime(char ch) {
    if (!connected_.load()) {
        return false;
    }
    if (ch == '!') {
        // Grbl 在任意字节边界消费 feed hold，不进入行缓冲；无需等普通写锁。
        return SendFeedHold();
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
        // 契约（hutuji_pipe.h）：Timeout 时必须写 -1，调用方不能在超时后读到旧码。
        if (error_code != nullptr) {
            *error_code = -1;
        }
        return WaitResult::Timeout;
    }
    RespItem item{};
    if (xQueueReceive(resp_queue_, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "等待 Grbl 应答超时 (%ums)", (unsigned)timeout_ms);
        if (error_code != nullptr) {
            *error_code = -1;
        }
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

}  // namespace hutuji
