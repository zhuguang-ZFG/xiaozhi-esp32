#include "plotter_provision.h"

#include <cstring>

#include <esp_log.h>
#include <esp_wifi.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "http.h"
#include "hutuji_pipe.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#define TAG "PlotterProvision"

namespace hutuji {

namespace {

// 与激活中转同级：一次性任务，HTTP(明文) + 跳网事件等待，8K 与下载路径同级。
constexpr uint32_t kProvisionTaskStack = 8192;

// 跳连事件位。
constexpr EventBits_t kJumpGotIp = BIT0;
constexpr EventBits_t kJumpDisconnected = BIT1;

// 跳连 GRBL_ESP 的握手重试：一次 DISCONNECTED 立即重试，盖住射频瞬态。
constexpr int kJumpConnectRetries = 2;

// 巡检期等既有发现的轮询步长；跳配验证期与回切等待的轮询步长。
constexpr uint32_t kGracePollMs = 1000;
constexpr uint32_t kVerifyPollMs = 2000;
constexpr uint32_t kRestorePollMs = 500;

void NotifyUser(const std::string& message) {
    Application::GetInstance().Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(message, 10000);
        }
    });
}

}  // namespace

PlotterProvision& PlotterProvision::GetInstance() {
    static PlotterProvision instance;
    return instance;
}

void PlotterProvision::JumpEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<PlotterProvision*>(arg);
    if (self == nullptr || self->jump_events_ == nullptr) {
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(self->jump_events_, kJumpGotIp);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(self->jump_events_, kJumpDisconnected);
    }
}

void PlotterProvision::OnHomeNetworkConnected() {
    // 新的户网连接纪元：自动尝试额度清零，取消挂起的退避重试，3s 后巡检。
    auto_attempts_ = 0;
    if (retry_timer_ != nullptr) {
        esp_timer_stop(retry_timer_);
    }
    if (patrol_timer_ == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &PlotterProvision::PatrolTimerCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "hutuji_patrol",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&args, &patrol_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "巡检定时器创建失败");
            return;
        }
    }
    esp_timer_stop(patrol_timer_);
    esp_timer_start_once(patrol_timer_, provision::kPatrolDelayMs * 1000ULL);
}

void PlotterProvision::RequestManual() {
    if (task_active_.load()) {
        NotifyUser("写字机配置进行中，请稍候");
        return;
    }
    // 管道在线 = 写字机已在家里网络上，没有要配置的对象。
    if (Pipe::GetInstance().IsConnected()) {
        NotifyUser("写字机已在线，无需重新配置");
        return;
    }
    pending_manual_.store(true);
    ScheduleAttempt(false, 0);
}

void PlotterProvision::PatrolTimerCb(void* arg) {
    auto* self = static_cast<PlotterProvision*>(arg);
    self->ScheduleAttempt(false, 0);
}

void PlotterProvision::ScheduleAttempt(bool manual, uint32_t delay_ms) {
    if (manual) {
        pending_manual_.store(true);
    }
    if (delay_ms > 0) {
        // 退避重试：到点从巡检重新走一遍。此刻状态机必然已收尾（task_active_
        // 在任务退出时清零），定时器回调里再抢单实例位。
        if (retry_timer_ == nullptr) {
            const esp_timer_create_args_t args = {
                .callback = &PlotterProvision::PatrolTimerCb,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "hutuji_prov_rt",
                .skip_unhandled_events = false,
            };
            if (esp_timer_create(&args, &retry_timer_) != ESP_OK) {
                ESP_LOGE(TAG, "重试定时器创建失败");
            }
            return;
        }
        esp_timer_stop(retry_timer_);
        esp_timer_start_once(retry_timer_, delay_ms * 1000ULL);
        return;
    }
    const bool want_manual = pending_manual_.exchange(false);
    if (task_active_.exchange(true)) {
        if (want_manual) {
            pending_manual_.store(true);  // 状态机在跑，手动意图留给下一轮
        }
        return;
    }
    if (xTaskCreate(&PlotterProvision::ProvisionTaskEntry, "plotter_prov", kProvisionTaskStack,
                    reinterpret_cast<void*>(want_manual ? 1 : 0), 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "配网任务创建失败");
        task_active_.store(false);
    }
}

void PlotterProvision::ProvisionTaskEntry(void* arg) {
    PlotterProvision::GetInstance().ProvisionTask(arg != nullptr);
    vTaskDelete(nullptr);
}

bool PlotterProvision::IsFactoryApVisible() {
    wifi_scan_config_t cfg = {};
    std::memcpy(cfg.ssid, provision::kPlotterApSsid, sizeof(provision::kPlotterApSsid));
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    // 户网连接态扫描会短暂离台；与别的扫描撞车（WifiStation 周期扫描）时
    // 退 1s 再来，三轮仍起不来本轮按「不在场」处理，不阻塞巡检。
    for (int i = 0; i < 3; ++i) {
        const esp_err_t err = esp_wifi_scan_start(&cfg, true);
        if (err == ESP_OK) {
            uint16_t num = 0;
            esp_wifi_scan_get_ap_num(&num);
            ESP_LOGI(TAG, "出厂热点扫描完成，命中 %u 个", (unsigned)num);
            return num > 0;
        }
        ESP_LOGW(TAG, "扫描启动失败 %s，1s 后重试（%d/3）", esp_err_to_name(err), i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool PlotterProvision::JumpToFactoryAp() {
    jump_events_ = xEventGroupCreate();
    if (jump_events_ == nullptr) {
        return false;
    }
    jump_netif_ = esp_netif_create_default_wifi_sta();
    if (jump_netif_ == nullptr) {
        vEventGroupDelete(jump_events_);
        jump_events_ = nullptr;
        return false;
    }
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &JumpEventHandler, this,
                                            &jump_wifi_handler_) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &JumpEventHandler, this,
                                            &jump_ip_handler_) != ESP_OK) {
        TeardownJump();
        return false;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "跳配 WiFi 启动失败: %s", esp_err_to_name(err));
        TeardownJump();
        return false;
    }

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), provision::kPlotterApSsid,
                 sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), provision::kPlotterApPassword,
                 sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "跳配配置写入失败");
        TeardownJump();
        return false;
    }

    const int64_t deadline =
        esp_timer_get_time() + static_cast<int64_t>(provision::kJumpConnectTimeoutMs) * 1000;
    for (int tried = 0; tried <= kJumpConnectRetries; ++tried) {
        xEventGroupClearBits(jump_events_, kJumpGotIp | kJumpDisconnected);
        if (esp_wifi_connect() != ESP_OK) {
            break;
        }
        while (true) {
            const int64_t left_us = deadline - esp_timer_get_time();
            if (left_us <= 0) {
                break;
            }
            const EventBits_t bits =
                xEventGroupWaitBits(jump_events_, kJumpGotIp | kJumpDisconnected, pdTRUE, pdFALSE,
                                    pdMS_TO_TICKS(static_cast<uint32_t>(left_us / 1000)));
            if (bits & kJumpGotIp) {
                ESP_LOGI(TAG, "已跳连写字机出厂热点");
                return true;
            }
            if (bits & kJumpDisconnected) {
                break;  // 交外层重试
            }
        }
    }
    ESP_LOGW(TAG, "跳连出厂热点失败/超时");
    TeardownJump();
    return false;
}

void PlotterProvision::TeardownJump() {
    // 与 WifiStation::Stop 同序：先摘事件（之后任何 WiFi 事件不再进本模块），
    // 再断连停驱动、毁 netif。做完即回到 WifiManager::StopStation 之后的状态，
    // StartStation 才能干净重建。
    if (jump_wifi_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, jump_wifi_handler_);
        jump_wifi_handler_ = nullptr;
    }
    if (jump_ip_handler_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, jump_ip_handler_);
        jump_ip_handler_ = nullptr;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (jump_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(jump_netif_);
        jump_netif_ = nullptr;
    }
    if (jump_events_ != nullptr) {
        vEventGroupDelete(jump_events_);
        jump_events_ = nullptr;
    }
}

bool PlotterProvision::SendCommands(provision::ProvisionFailure* failure) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        *failure = provision::ProvisionFailure::ApAuthFailed;
        return false;
    }
    const auto sequence = provision::BuildCommandSequence(home_ssid_, home_password_);
    for (size_t i = 0; i < sequence.size(); ++i) {
        const bool is_restart = (i == sequence.size() - 1);
        // 每条写命令一次传输级重试：跳配链路是跨房间的临时射频，瞬断不值得整轮重来。
        for (int tried = 0; tried < 2; ++tried) {
            auto http = network->CreateHttp(3);
            if (http == nullptr) {
                *failure = provision::ProvisionFailure::ApAuthFailed;
                return false;
            }
            http->SetTimeout(static_cast<int>(provision::kHttpTimeoutMs));
            const std::string url = provision::BuildCommandUrl(sequence[i]);
            // 脱敏：ESP101 的 URL 带户网密码明文，日志只记命令序号，不记 URL。
            if (!http->Open("GET", url)) {
                http->Close();
                if (is_restart) {
                    return true;  // RESTART 发出即可能立刻重启，连不上按已发出处理
                }
                if (tried == 0) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                *failure = provision::ProvisionFailure::ApAuthFailed;
                return false;
            }
            const int status = http->GetStatusCode();
            const std::string body = http->ReadAll();
            http->Close();
            const auto result = provision::ClassifyEspResponse(status, body);
            if (is_restart) {
                if (provision::IsRestartOutcomeAcceptable(result)) {
                    return true;
                }
                ESP_LOGW(TAG, "RESTART 被写字机拒绝");
                *failure = provision::ProvisionFailure::CommandRejected;
                return false;
            }
            if (result == provision::EspCmdResult::Ok) {
                break;
            }
            if (result == provision::EspCmdResult::Rejected) {
                ESP_LOGW(TAG, "第 %u 条配置命令被拒", (unsigned)(i + 1));
                *failure = provision::ProvisionFailure::CommandRejected;
                return false;
            }
            if (tried == 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            *failure = provision::ProvisionFailure::ApAuthFailed;
            return false;
        }
    }
    return true;
}

bool PlotterProvision::WaitHomeBack() {
    auto& wifi = WifiManager::GetInstance();
    for (uint32_t waited = 0; waited < provision::kRestoreTimeoutMs; waited += kRestorePollMs) {
        if (wifi.IsConnected()) {
            ESP_LOGI(TAG, "已回切户网");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kRestorePollMs));
    }
    ESP_LOGW(TAG, "回切户网超时（station 仍在自动重试，不长期离线）");
    return false;
}

void PlotterProvision::FinishWith(bool manual, bool ok, provision::ProvisionFailure failure,
                                  int attempt) {
    if (ok) {
        auto_attempts_ = 0;
        NotifyUser("写字机已连上家里网络，可以开始画了");
        return;
    }
    ESP_LOGW(TAG, "写字机配网失败（第 %d 次，%s ）", attempt, provision::DescribeFailure(failure));
    if (manual || !provision::IsTransient(failure) || attempt >= provision::kMaxAutoAttempts) {
        // 手动触发、确定性失败、自动额度耗尽：都转人工话术，不重试风暴。
        NotifyUser(provision::DescribeFailure(failure));
        return;
    }
    ScheduleAttempt(false, provision::AutoRetryDelayMs(attempt));
}

void PlotterProvision::ProvisionTask(bool manual) {
    // 统一出口：回切户网（如已停）+ 结果上报 + 清 busy_（跳窗标志）+
    // 清 task_active_（单实例位）。验证轮询在 busy_ 清零之后——验证靠 Pipe 跑。
    provision::ProvisionFailure failure = provision::ProvisionFailure::ApNotInRange;
    const int attempt = ++auto_attempts_;
    bool station_stopped = false;
    bool jumped = false;
    auto finish = [&](bool ok) {
        if (jumped) {
            TeardownJump();
            jumped = false;
        }
        if (station_stopped) {
            WifiManager::GetInstance().StartStation();
            station_stopped = false;
            if (!WaitHomeBack() && ok) {
                failure = provision::ProvisionFailure::HomeWifiRestoreFailed;
                ok = false;
            }
        }
        busy_.store(false);  // 跳窗结束；验证期 Pipe 必须能跑
        if (ok) {
            // 验证：写字机重启约 8~15s，轮询管道接管结果。
            for (uint32_t waited = 0; waited < provision::kVerifyTimeoutMs;
                 waited += kVerifyPollMs) {
                if (Pipe::GetInstance().IsConnected()) {
                    FinishWith(manual, true, failure, attempt);
                    task_active_.store(false);
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(kVerifyPollMs));
            }
            failure = provision::ProvisionFailure::NotOnlineAfterRestart;
            ok = false;
        }
        FinishWith(manual, false, failure, attempt);
        task_active_.store(false);
    };

    // 1) 先等既有发现：写字机已在户网时管道几秒内就连上，不扫网不跳配。
    if (!manual) {
        for (uint32_t waited = 0; waited < provision::kPipeGraceMs; waited += kGracePollMs) {
            if (Pipe::GetInstance().IsConnected()) {
                ESP_LOGI(TAG, "写字机已在户网，巡检退出");
                task_active_.store(false);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(kGracePollMs));
        }
    }

    // 2) 读回户网凭据（当前连接 SSID 对应的表项），先过写字机侧校验边界。
    {
        const std::string current = WifiManager::GetInstance().GetSsid();
        for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
            if (item.ssid == current) {
                home_ssid_ = item.ssid;
                home_password_ = item.password;
                break;
            }
        }
        if (home_ssid_.empty()) {
            ESP_LOGW(TAG, "户网凭据读不回（当前 SSID 不在列表），退出");
            task_active_.store(false);
            return;
        }
        if (provision::ValidateHomeCredentials(home_ssid_, home_password_) !=
            provision::CredentialError::None) {
            failure = provision::ProvisionFailure::InvalidCredentials;
            finish(false);
            return;
        }
    }

    // 3) 出厂热点在场才跳配：自动路径下不在场 = 无事可做，静默退出。
    if (!IsFactoryApVisible()) {
        failure = provision::ProvisionFailure::ApNotInRange;
        if (manual) {
            finish(false);
        } else {
            task_active_.store(false);
        }
        return;
    }

    // 4) 跳配。busy_ 覆盖跳网+回切全程：Pipe 在此期间歇工。
    ESP_LOGI(TAG, "发现待配置写字机，开始跳配（第 %d 次，%s）", attempt, manual ? "手动" : "自动");
    busy_.store(true);
    WifiManager::GetInstance().StopStation();
    station_stopped = true;
    if (!JumpToFactoryAp()) {
        // 失败路径 JumpToFactoryAp 内部已 TeardownJump，只剩恢复户网。
        failure = provision::ProvisionFailure::ApAuthFailed;
        finish(false);
        return;
    }
    jumped = true;
    if (!SendCommands(&failure)) {
        finish(false);
        return;
    }
    finish(true);
}

}  // namespace hutuji
