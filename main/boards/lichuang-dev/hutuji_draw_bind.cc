#include "hutuji_draw_bind.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "http.h"
#include "hutuji_bind_identity.h"
#include "hutuji_pipe.h"
#include "hutuji_recovery_core.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>
#include <string>
#include <utility>

namespace hutuji {
namespace {

constexpr const char* kTag = "HutujiDrawBind";
constexpr const char* kAnnounceUrl =
    "https://hutuji.donglicao.com/draw-upload/api/device/bind/announce";
constexpr const char* kChallengeUrl =
    "https://hutuji.donglicao.com/draw-upload/api/device/bind/challenge";
// 4096 栈曾在 COM14 首次 TLS 握手溢出；与 activation_relay / hutuji_job 同口径。
constexpr uint32_t kTaskStack = 8192;
constexpr int64_t kWindowUs = 10LL * 60 * 1000000;
// 首分钟快轮询；随后降低握手频率，并让出听/说窗口，避免抢 AFE/TTS。
constexpr int kFastRounds = 12;
constexpr int kFastMs = 5000;
constexpr int kSlowMs = 30000;

enum class Attempt { Retry, Pending, Bound, Expired, Rejected };
std::mutex g_run_mutex;
BindIdentityRun g_run;
Display* g_display = nullptr;
TaskHandle_t g_worker = nullptr;

std::string TrimToken(std::string token) {
    const auto first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    token = token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
    if (token.rfind("Bearer ", 0) == 0)
        return TrimToken(token.substr(7));
    return token;
}

std::string DeviceMac() {
    std::string mac = SystemInfo::GetMacAddress();
    for (char& ch : mac) {
        if (ch >= 'A' && ch <= 'F')
            ch += 'a' - 'A';
    }
    return mac;
}

std::string ReadDeviceToken() {
    // messaging 优先；空凭据仍能凭设备签名绑定，由服务端自动铸 messaging token。
    Settings messaging("messaging", false);
    const std::string token = TrimToken(messaging.GetString("token"));
    if (!token.empty())
        return token;
    Settings websocket("websocket", false);
    return TrimToken(websocket.GetString("token"));
}

void AddIdentityFields(cJSON* root, const BindIdentitySession& session) {
    cJSON_AddNumberToObject(root, "identity_version", kBindIdentityVersion);
    cJSON_AddStringToObject(root, "device_id", session.device_id.c_str());
    cJSON_AddStringToObject(root, "public_key", session.public_key.c_str());
    cJSON_AddStringToObject(root, "nonce", session.nonce.c_str());
    cJSON_AddStringToObject(root, "bind_code", session.bind_code.c_str());
    cJSON_AddStringToObject(root, "mac", DeviceMac().c_str());
}

Attempt Post(cJSON* root, const char* url, std::string& response) {
    response.clear();
    if (root == nullptr)
        return Attempt::Retry;
    char* raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (raw == nullptr)
        return Attempt::Retry;
    std::string body(raw);
    cJSON_free(raw);
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr)
        return Attempt::Retry;
    auto http = network->CreateHttp(3);
    if (http == nullptr)
        return Attempt::Retry;
    http->SetTimeout(8000);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::move(body));
    if (!http->Open("POST", url)) {
        http->Close();
        return Attempt::Retry;
    }
    const int status = http->GetStatusCode();
    bool complete = false;
    char buffer[256];
    while (status == 200 && response.size() < 1024) {
        const int count = http->Read(buffer, sizeof(buffer));
        if (count <= 0) {
            complete = count == 0;
            break;
        }
        response.append(buffer, static_cast<size_t>(count));
    }
    http->Close();
    if (status == 410)
        return Attempt::Expired;
    if (status == 400 || status == 401 || status == 403 || status == 409 || status == 422) {
        ESP_LOGW(kTag, "bind request rejected status=%d", status);
        return Attempt::Rejected;
    }
    return status == 200 && complete && response.size() < 1024 ? Attempt::Pending : Attempt::Retry;
}

Attempt FetchChallenge(const BindIdentitySession& session, std::string& challenge) {
    challenge.clear();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr)
        return Attempt::Retry;
    AddIdentityFields(root, session);
    std::string response;
    const Attempt result = Post(root, kChallengeUrl, response);
    if (result != Attempt::Pending)
        return result;
    cJSON* json = cJSON_Parse(response.c_str());
    const cJSON* value = json != nullptr ? cJSON_GetObjectItem(json, "challenge") : nullptr;
    if (cJSON_IsString(value) && value->valuestring != nullptr &&
        BindCanonicalBase64Url(value->valuestring, 32)) {
        challenge = value->valuestring;
    }
    cJSON_Delete(json);
    return challenge.empty() ? Attempt::Retry : Attempt::Pending;
}

Attempt AnnounceOnce(const BindIdentitySession& session, std::string& challenge) {
    if (challenge.empty()) {
        const Attempt result = FetchChallenge(session, challenge);
        if (result != Attempt::Pending)
            return result;
    }
    const std::string token = ReadDeviceToken();
    const std::string sku = Pipe::GetInstance().IsNopaperMachine() ? "nopaper" : "paper";
    std::string signature;
    if (!SignBindIdentity(session, challenge, DeviceMac(), sku, token, signature))
        return Attempt::Retry;
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr)
        return Attempt::Retry;
    AddIdentityFields(root, session);
    cJSON_AddStringToObject(root, "challenge", challenge.c_str());
    cJSON_AddStringToObject(root, "signature", signature.c_str());
    cJSON_AddStringToObject(root, "device_token", token.c_str());
    cJSON_AddStringToObject(root, "plotter_sku", sku.c_str());
    std::string response;
    const Attempt result = Post(root, kAnnounceUrl, response);
    if (result != Attempt::Pending)
        return result;
    cJSON* json = cJSON_Parse(response.c_str());
    const cJSON* bound = json != nullptr ? cJSON_GetObjectItem(json, "bound") : nullptr;
    const Attempt answer = cJSON_IsTrue(bound)    ? Attempt::Bound
                           : cJSON_IsFalse(bound) ? Attempt::Pending
                                                  : Attempt::Retry;
    cJSON_Delete(json);
    return answer;
}

void EndRun(uint64_t generation, bool success, const char* message) {
    Display* display = nullptr;
    bool manual = false;
    {
        std::lock_guard<std::mutex> lock(g_run_mutex);
        if (!g_run.Current(generation))
            return;
        if (success) {
            if (!FinishBindIdentitySession(g_run.session.nonce))
                return;
            Settings settings("hutuji", true);
            settings.SetInt("auto_bound", 1);
        }
        display = g_display;
        manual = g_run.mode == BindRunMode::Manual;
        g_run.Complete(generation);
    }
    Application::GetInstance().Schedule([generation, display, manual, message]() {
        {
            std::lock_guard<std::mutex> lock(g_run_mutex);
            if (g_run.generation != generation)
                return;
        }
        if (display != nullptr) {
            if (manual)
                display->HideProvisioningQr();
            display->ShowNotification(message, 5000);
        }
    });
}

void RunWorker() {
    uint64_t generation = 0;
    int64_t deadline = 0;
    int round = 0;
    std::string challenge;
    for (;;) {
        BindIdentityRun current;
        {
            std::lock_guard<std::mutex> lock(g_run_mutex);
            if (g_run.mode == BindRunMode::Stopped) {
                // 句柄摘除与启动检查同锁；外部只发通知，不删除正在 HTTP/NVS 中的任务。
                g_worker = nullptr;
                return;
            }
            current = g_run;
        }
        if (current.generation != generation) {
            generation = current.generation;
            deadline = esp_timer_get_time() + kWindowUs;
            challenge.clear();
            round = 0;
        }
        if (esp_timer_get_time() >= deadline) {
            EndRun(generation, false, "绑定暂未完成，请重新打开配网或绑定");
            continue;
        }
        const auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        const Attempt result = AnnounceOnce(current.session, challenge);
        if (result == Attempt::Bound)
            EndRun(generation, true, "已绑定呼图账号");
        else if (result == Attempt::Expired)
            EndRun(generation, false, "二维码已过期，请重新打开配网或绑定");
        else if (result == Attempt::Rejected)
            EndRun(generation, false, "绑定信息已变化，请重新扫描设备二维码");
        // Stop/新二维码会唤醒等待，不能因旧 worker 睡 30 秒而漏掉唯一的激活回调。
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(round++ < kFastRounds ? kFastMs : kSlowMs));
    }
}

void WorkerTask(void*) {
    // vTaskDelete 不展开 C++ 栈；先正常返回，释放 HTTP/字符串等局部资源。
    RunWorker();
    vTaskDelete(nullptr);
}

bool QueueSession(const BindIdentitySession& session, BindRunMode mode, Display* display) {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    if (mode == BindRunMode::Headless && g_run.mode == BindRunMode::Manual)
        return true;
    if (g_run.mode == mode && g_run.session.nonce == session.nonce && g_worker != nullptr)
        return true;
    g_run.Request(session, mode);
    if (display != nullptr)
        g_display = display;
    if (g_worker != nullptr) {
        xTaskNotifyGive(g_worker);
    } else if (xTaskCreate(WorkerTask, "hutuji_bind", kTaskStack, nullptr, 3, &g_worker) !=
               pdPASS) {
        g_worker = nullptr;
        g_run.Stop();
        ESP_LOGW(kTag, "bind worker unavailable");
        return false;
    }
    return true;
}

void StopRun(bool headless_only) {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    if (headless_only && g_run.mode == BindRunMode::Manual)
        return;
    g_run.Stop();
    if (g_worker != nullptr)
        xTaskNotifyGive(g_worker);
}

}  // namespace

std::string BuildIdentityWifiQrPayload(const std::string& ssid, const std::string& mac) {
    StopRun(false);
    BindIdentitySession session;
    if (!BeginBindIdentitySession(session)) {
        ESP_LOGW(kTag, "identity QR unavailable");
        return "";
    }
    return BuildOpenHotspotWifiQrPayload(ssid, mac) + BindIdentityQuery(session);
}

void StartAutoBindHeadless(Display* display) {
    BindIdentitySession session;
    // 开机/切网复用持久化会话，不另造用户尚未扫描的 nonce。
    if (LoadBindIdentitySession(session))
        QueueSession(session, BindRunMode::Headless, display);
}

void StopAutoBindHeadless() { StopRun(true); }

void StartDrawBind(Display* display) {
    if (display == nullptr)
        return;
    StopRun(false);
    BindIdentitySession session;
    if (!BeginBindIdentitySession(session) ||
        !QueueSession(session, BindRunMode::Manual, display)) {
        display->HideProvisioningQr();
        display->ShowNotification("绑定暂不可用，请重启后重试", 5000);
        return;
    }
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(g_run_mutex);
        generation = g_run.generation;
    }
    const std::string url = "https://hutuji.donglicao.com/draw-upload/bind?c=" + session.bind_code +
                            "&m=" + UrlEncodeQueryComponent(DeviceMac()) +
                            BindIdentityQuery(session);
    const std::string hint = "绑定码 " + session.bind_code + "\n10 分钟内手机扫码并完成绑定";
    Application::GetInstance().Schedule([display, url, hint, generation]() {
        {
            std::lock_guard<std::mutex> lock(g_run_mutex);
            if (!g_run.Current(generation))
                return;
        }
        display->ShowProvisioningQr(url, hint);
    });
}

void StopDrawBind(Display* display) {
    StopRun(false);
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(g_run_mutex);
        generation = g_run.generation;
        if (display == nullptr)
            display = g_display;
    }
    Application::GetInstance().Schedule([display, generation]() {
        {
            std::lock_guard<std::mutex> lock(g_run_mutex);
            if (generation != g_run.generation)
                return;
        }
        if (display != nullptr)
            display->HideProvisioningQr();
    });
}

bool IsDrawBindActive() {
    std::lock_guard<std::mutex> lock(g_run_mutex);
    return g_run.mode == BindRunMode::Manual;
}

}  // namespace hutuji
