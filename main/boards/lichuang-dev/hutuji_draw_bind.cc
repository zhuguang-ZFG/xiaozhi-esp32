#include "hutuji_draw_bind.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "http.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <string>

namespace hutuji {
namespace {

constexpr const char* kTag = "HutujiDrawBind";
constexpr const char* kAnnounceUrl =
    "https://hutuji.donglicao.com/draw-upload/api/device/bind/announce";
constexpr const char* kSessionUrlBase =
    "https://hutuji.donglicao.com/draw-upload/api/device/bind/session?bind_code=";
// HTTPS/mbedTLS 与 activation_relay / hutuji_job 同口径；此前 Poll 单独 4096
// 会在首次轮询 TLS 时栈溢出重启（COM14 实锤：hutuji_bind_pol stack overflow）。
constexpr uint32_t kTaskStack = 8192;
constexpr int kPollIntervalMs = 3000;
constexpr int kPollMaxAttempts = 200;  // ~10 分钟

Display* g_display = nullptr;
std::string g_bind_code;
bool g_active = false;
TaskHandle_t g_worker = nullptr;

std::string GenerateBindCode() {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string code;
    code.resize(6);
    for (int i = 0; i < 6; ++i) {
        code[i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
    }
    return code;
}

std::string ReadDeviceToken() {
    // 设备消息 API 须 purpose=messaging JWT（控制台「主题配置 / assets-generator」URL
    // 的 token 参数）。websocket.token 是 WSS 音频通道凭据，不能用于 push（401）。
    {
        Settings messaging("messaging", false);
        std::string token = messaging.GetString("token");
        if (token.rfind("Bearer ", 0) == 0) {
            token = token.substr(7);
        }
        if (token.size() >= 8) {
            return token;
        }
    }
    ESP_LOGW(kTag, "messaging token missing in NVS; announce may fail device push probe");
    Settings settings("websocket", false);
    std::string token = settings.GetString("token");
    if (token.rfind("Bearer ", 0) == 0) {
        token = token.substr(7);
    }
    return token;
}

bool AnnounceOnce(const std::string& code, bool* bound_out) {
    const std::string token = ReadDeviceToken();
    if (token.size() < 8) {
        ESP_LOGW(kTag, "device token missing, skip announce mac=%s",
                 SystemInfo::GetMacAddress().c_str());
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(kTag, "no network, skip announce");
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "bind_code", code.c_str());
    cJSON_AddStringToObject(root, "device_token", token.c_str());
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGW(kTag, "CreateHttp failed");
        cJSON_free(body);
        return false;
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(body));
    cJSON_free(body);
    bool ok = false;
    if (!http->Open("POST", kAnnounceUrl)) {
        ESP_LOGW(kTag, "announce open failed mac=%s", SystemInfo::GetMacAddress().c_str());
    } else {
        const int status = http->GetStatusCode();
        ESP_LOGI(kTag, "announce mac=%s status=%d", SystemInfo::GetMacAddress().c_str(), status);
        ok = (status >= 200 && status < 300);
        if (ok && bound_out != nullptr) {
            // 2026-09-08 起 portal 在响应里回 bound：该 MAC 已被用户认领并完成撮合。
            // 老 portal 无此字段 → 解析不到即 false，行为与旧版一致。
            char buf[192] = {};
            const int n = http->Read(buf, sizeof(buf) - 1);
            if (n > 0) {
                cJSON* resp = cJSON_Parse(buf);
                if (resp != nullptr) {
                    *bound_out = cJSON_IsTrue(cJSON_GetObjectItem(resp, "bound"));
                    cJSON_Delete(resp);
                }
            }
        }
        http->Close();
    }
    return ok;
}

bool PollConsumedOnce(const std::string& code) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    const std::string url = std::string(kSessionUrlBase) + code;
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        return false;
    }
    if (!http->Open("GET", url)) {
        return false;
    }
    const int status = http->GetStatusCode();
    char buf[256] = {};
    const int n = http->Read(buf, sizeof(buf) - 1);
    http->Close();
    if (status != 200 || n <= 0) {
        return false;
    }
    const std::string body(buf, static_cast<size_t>(n));
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    const cJSON* st = cJSON_GetObjectItem(root, "status");
    const char* state = cJSON_IsString(st) ? st->valuestring : "";
    const bool consumed = std::strcmp(state, "consumed") == 0;
    cJSON_Delete(root);
    return consumed;
}

void BindWorkerTask(void* /*arg*/) {
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    const std::string code = g_bind_code;
    // announce 单发时代 TLS 一抖整码作废（用户输码必撞「无效或已过期」2026-09-07 JJAUAL 实锤），
    // 故并入轮询环持续重试直到 200；与 consumed 轮询共享同一 10 分钟上限。
    bool announced = false;
    for (int i = 0; i < kPollMaxAttempts && g_active; ++i) {
        if (!announced) {
            announced = AnnounceOnce(code, nullptr);
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
        if (!g_active) {
            break;
        }
        if (!announced) {
            continue;  // 没 announce 成功前查会话无意义（服务端必然 missing）
        }
        if (!PollConsumedOnce(code)) {
            continue;
        }
        ESP_LOGI(kTag, "bind session consumed mac=%s", SystemInfo::GetMacAddress().c_str());
        Application::GetInstance().Schedule([]() {
            if (g_display != nullptr) {
                g_display->ShowNotification("绑定成功", 5000);
                StopDrawBind(g_display);
            }
        });
        break;
    }
    if (!announced) {
        ESP_LOGW(kTag, "bind window ended without successful announce mac=%s",
                 SystemInfo::GetMacAddress().c_str());
    }
    if (g_worker == self) {
        g_worker = nullptr;
    }
    vTaskDelete(nullptr);
}

void QueueWorker() {
    if (g_worker != nullptr) {
        ESP_LOGW(kTag, "bind worker already running, skip recreate");
        return;
    }
    if (xTaskCreate(BindWorkerTask, "hutuji_bind", kTaskStack, nullptr, 3, &g_worker) !=
        pdPASS) {
        ESP_LOGW(kTag, "bind worker create failed");
        g_worker = nullptr;
    }
}

// ---- 一次扫码合一流程（2026-09-08 定案）：无头 announce ----
// 抽屉流要用户开抽屉输码；合一流程里用户只扫配网码，设备侧 announce 后
// 由 portal 按 MAC 与小程序认领撮合，响应 bound:true 即完成。
constexpr int kAutoBindIntervalMs = 5000;    // 无人在等结果，比抽屉流 3s 温和
constexpr int kAutoBindMaxAttempts = 120;    // ~10 分钟窗口，盖住激活重试环

std::string g_auto_code;
bool g_auto_active = false;
TaskHandle_t g_auto_worker = nullptr;

void AutoBindWorkerTask(void* /*arg*/) {
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    const std::string code = g_auto_code;
    bool bound = false;
    for (int i = 0; i < kAutoBindMaxAttempts && g_auto_active && !bound; ++i) {
        // 每轮重发同一码（幂等 upsert、顺带刷新会话 TTL），直到 portal 回 bound
        // 或窗口结束；token 未就位（激活未完成）时 AnnounceOnce 快速失败等下轮。
        AnnounceOnce(code, &bound);
        if (!bound) {
            vTaskDelay(pdMS_TO_TICKS(kAutoBindIntervalMs));
        }
    }
    if (bound) {
        ESP_LOGI(kTag, "auto bind matched mac=%s", SystemInfo::GetMacAddress().c_str());
        Application::GetInstance().Schedule([]() {
            if (g_display != nullptr) {
                g_display->ShowNotification("已绑定呼图账号", 5000);
            }
        });
    } else if (g_auto_active) {
        ESP_LOGW(kTag, "auto bind window ended unbound mac=%s",
                 SystemInfo::GetMacAddress().c_str());
    }
    g_auto_active = false;
    if (g_auto_worker == self) {
        g_auto_worker = nullptr;
    }
    vTaskDelete(nullptr);
}

}  // namespace

void StartAutoBindHeadless(Display* display) {
    if (g_active) {
        return;  // 抽屉绑定流在跑：announce 归它管（同样会被撮合），不抢 worker
    }
    if (g_auto_worker != nullptr) {
        return;  // 本窗口已在跑：激活完成事件每次开机都到，重复调用安全
    }
    if (display != nullptr) {
        g_display = display;
    }
    g_auto_code = GenerateBindCode();
    g_auto_active = true;
    if (xTaskCreate(AutoBindWorkerTask, "hutuji_autobind", kTaskStack, nullptr, 3,
                    &g_auto_worker) != pdPASS) {
        ESP_LOGW(kTag, "auto bind worker create failed");
        g_auto_worker = nullptr;
        g_auto_active = false;
        return;
    }
    ESP_LOGI(kTag, "auto bind headless started mac=%s", SystemInfo::GetMacAddress().c_str());
}

void StopAutoBindHeadless() {
    g_auto_active = false;  // worker 下个周期自查退出，不在外部杀任务
}


void StartDrawBind(Display* display) {
    if (display == nullptr) {
        return;
    }
    StopAutoBindHeadless();  // 抽屉流接管 announce，无头 worker 下个周期退出
    g_display = display;
    g_bind_code = GenerateBindCode();
    g_active = true;
    const std::string url =
        std::string("https://hutuji.donglicao.com/draw-upload/bind?c=") + g_bind_code;
    const std::string hint = std::string("绑定码 ") + g_bind_code + "\n10 分钟内手机扫码并完成绑定";
    Application::GetInstance().Schedule([display, url, hint]() {
        display->ShowProvisioningQr(url, hint);
    });
    QueueWorker();
    ESP_LOGI(kTag, "bind flow started mac=%s", SystemInfo::GetMacAddress().c_str());
}

void StopDrawBind(Display* display) {
    (void)display;
    g_active = false;
    g_bind_code.clear();
    if (g_display != nullptr) {
        Application::GetInstance().Schedule([]() {
            if (g_display != nullptr) {
                g_display->HideProvisioningQr();
            }
        });
    }
}

bool IsDrawBindActive() { return g_active; }

}  // namespace hutuji
