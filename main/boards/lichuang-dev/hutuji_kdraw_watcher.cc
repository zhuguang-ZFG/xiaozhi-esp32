#include "hutuji_kdraw_watcher.h"

#include "application.h"
#include "board.h"
#include "http.h"
#include "system_info.h"

#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <lwip/sockets.h>

namespace hutuji {
namespace kdraw {
namespace {

constexpr const char* kTag = "HutujiKdraw";
// 纯 UDP 收包 + 解析，无 TLS；6KiB 静态栈余量充足。
constexpr uint32_t kTaskStackWords = 6144 / sizeof(StackType_t);
constexpr UBaseType_t kTaskPriority = 1;  // 低于音频/网络主路径
constexpr int kBeaconPort = 2325;
// Idle 稳定窗：覆盖页间换纸（Changing=On 段不计时）与短促 Hold 抖动。
constexpr int64_t kStableUs = 2500 * 1000;
// 收包缓冲：payload 设计 48B 有界，给足裕量。
constexpr size_t kRecvBuf = 160;

constexpr const char* kKdrawDoneUrl =
    "https://hutuji.donglicao.com/draw-upload/api/device/kdraw/done";
TaskHandle_t g_worker = nullptr;
StaticTask_t g_worker_tcb;
StackType_t g_worker_stack[kTaskStackWords];
volatile bool g_started = false;

// —— 完成判定状态机（单任务独占，无锁）——
bool g_saw_run = false;        // 本次任务见过 Run？
int64_t g_idle_since_us = 0;   // Idle 且 !Changing 的起点；0=不在稳定窗
bool g_fired = false;          // 本次 Run 已发完成（防连发）

/** state 形如 `Run` / `Idle` / `Hold:0` / `Door:1`：取 `:` 前的主状态。 */
std::string_view MainState(std::string_view token) {
    size_t colon = token.find(':');
    if (colon != std::string_view::npos) {
        token = token.substr(0, colon);
    }
    return token;
}

/** 解析 `<Run|Changing=Off|Seq=17>`；字段顺序不硬编码，按名匹配。 */
bool ParseBeacon(const char* line, size_t len, std::string_view* state_out, bool* changing_out) {
    if (len < 2 || line[0] != '<' || line[len - 1] != '>') {
        return false;
    }
    std::string_view body(line + 1, len - 2);
    bool have_state = false;
    bool changing = false;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('|', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        std::string_view field = body.substr(start, end - start);
        start = end + 1;
        if (field.empty()) {
            continue;
        }
        if (field.rfind("Changing=", 0) == 0) {
            changing = (field.substr(9) == "On");
            continue;
        }
        if (field.rfind("Seq=", 0) == 0) {
            continue;  // 观察端不依赖 Seq；仅调试价值
        }
        if (!have_state) {
            *state_out = MainState(field);
            have_state = true;
        }
    }
    if (!have_state) {
        return false;
    }
    *changing_out = changing;
    return true;
}

/** 云端通知：设备侧 MCP 主动推送（与 Job::Notify 同形；Notify 本身 private）。 */
void NotifyCloud(const char* text) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "notifications/message");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "level", "info");
    cJSON_AddStringToObject(params, "data", text);
    cJSON_AddItemToObject(root, "params", params);
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (str == nullptr) {
        return;
    }
    Application::GetInstance().SendMcpMessage(str);
    cJSON_free(str);
}

/** 完成播报：本地提示音 + 屏显 + 云端通知（LLM 会转述给用户）。 */
void AnnounceDone() {
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().Alert("奎享完成", "奎享画完了，可以取纸了", "happy",
                                         Lang::Sounds::OGG_SUCCESS);
    });
    // Schedule 异步执行：lambda 不捕获局部引用，直接取单例。
    NotifyCloud("奎享任务完成：写字机已完成奎享的下发任务。");
    // portal 反向通知（protocol §10.4.17）：MAC 绑定闸 + 小程序一次性订阅消息。
    auto network = Board::GetInstance().GetNetwork();
    if (network != nullptr) {
        cJSON* root = cJSON_CreateObject();
        if (root != nullptr) {
            cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
            char* body = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (body != nullptr) {
                auto http = network->CreateHttp(5);
                if (http != nullptr) {
                    http->SetHeader("Content-Type", "application/json");
                    http->SetContent(std::string(body));
                    if (!http->Open("POST", kKdrawDoneUrl)) {
                        ESP_LOGW(kTag, "kdraw done post open failed");
                    } else {
                        ESP_LOGI(kTag, "kdraw done post status=%d", http->GetStatusCode());
                        http->Close();
                    }
                }
                cJSON_free(body);
            }
        }
    }
    ESP_LOGI(kTag, "kdraw done detected");
}

void WorkerTask(void* /*arg*/) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(kTag, "socket failed errno=%d", errno);
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kBeaconPort);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(kTag, "bind :%d failed errno=%d", kBeaconPort, errno);
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    // 1s 超时轮询：稳定窗计时与收包共用一个循环。
    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(kTag, "listening udp :%d", kBeaconPort);

    char buf[kRecvBuf];
    while (true) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        int64_t now = esp_timer_get_time();
        if (n <= 0) {
            // 超时仅推进稳定窗（无包时机器可能已关机，不判完成）。
            if (g_saw_run && !g_fired && g_idle_since_us != 0 &&
                now - g_idle_since_us >= kStableUs) {
                g_fired = true;
                AnnounceDone();
            }
            continue;
        }
        buf[n] = '\0';
        std::string_view state;
        bool changing = false;
        if (!ParseBeacon(buf, static_cast<size_t>(n), &state, &changing)) {
            continue;
        }
        if (state == "Run") {
            g_saw_run = true;
            g_fired = false;
            g_idle_since_us = 0;
            continue;
        }
        if (state == "Alarm" || state == "Check") {
            // 任务失败态：不庆祝完成，回到待新任务。
            g_saw_run = false;
            g_idle_since_us = 0;
            continue;
        }
        if (g_saw_run && !g_fired) {
            if (state == "Idle" && !changing) {
                if (g_idle_since_us == 0) {
                    g_idle_since_us = now;
                } else if (now - g_idle_since_us >= kStableUs) {
                    g_fired = true;
                    AnnounceDone();
                }
            } else {
                // Hold/Jog/Home/Door 或换纸中：稳定窗清零重计。
                g_idle_since_us = 0;
            }
        }
    }
}

}  // namespace

void Start() {
    if (g_started) {
        return;
    }
    g_started = true;
    g_worker = xTaskCreateStatic(WorkerTask, "hutuji_kdraw", kTaskStackWords, nullptr,
                                 kTaskPriority, g_worker_stack, &g_worker_tcb);
    if (g_worker == nullptr) {
        ESP_LOGE(kTag, "worker create failed");
        g_started = false;
        return;
    }
    ESP_LOGI(kTag, "kdraw watcher started");
}

}  // namespace kdraw
}  // namespace hutuji
