#include "hutuji_kdraw_watcher.h"
#include "hutuji_kdraw_core.h"
#include "hutuji_pipe.h"

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
#include <atomic>
#include <cerrno>
#include <cstring>
#include <lwip/sockets.h>

namespace hutuji {
namespace kdraw {
namespace {

constexpr const char* kTag = "HutujiKdraw";
// 本任务还执行完成通知 HTTPS；IDF 栈深度单位为字节，不能传 vanilla 字数。
constexpr uint32_t kTaskStackBytes = 6144;
constexpr UBaseType_t kTaskPriority = 1;  // 低于音频/网络主路径
constexpr int kBeaconPort = 2325;
// 收包缓冲：payload 设计 48B 有界，给足裕量。
constexpr size_t kRecvBuf = 160;

constexpr const char* kKdrawDoneUrl =
    "https://hutuji.donglicao.com/draw-upload/api/device/kdraw/done";
TaskHandle_t g_worker = nullptr;
StaticTask_t g_worker_tcb;
StackType_t g_worker_stack[kTaskStackBytes / sizeof(StackType_t)];
std::atomic<bool> g_started{false};

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
    for (;;) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGW(kTag, "socket failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(kBeaconPort);
        timeval tv{};
        tv.tv_sec = 1;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            ESP_LOGW(kTag, "beacon socket setup failed errno=%d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(kTag, "listening udp :%d", kBeaconPort);
        CompletionTracker tracker;
        char buf[kRecvBuf];
        for (;;) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            int n =
                recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
            const uint64_t now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
            tracker.Expire(now);
            if (n < 0) {
                // 静默只会使证据过期，绝不能据此宣布取纸。
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                break;
            }
            if (n == 0 || n >= static_cast<int>(sizeof(buf)) || from.sin_family != AF_INET ||
                ntohs(from.sin_port) != kBeaconPort)
                continue;
            Beacon beacon;
            if (!ParseBeacon(std::string_view(buf, static_cast<size_t>(n)), beacon))
                continue;
            const uint32_t expected = Pipe::GetInstance().GetKnownPeerIp();
            if (tracker.Observe(from.sin_addr.s_addr, expected, beacon, now))
                AnnounceDone();
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

}  // namespace

void Start() {
    if (g_started.exchange(true)) {
        return;
    }
    g_worker = xTaskCreateStatic(WorkerTask, "hutuji_kdraw", sizeof(g_worker_stack), nullptr,
                                 kTaskPriority, g_worker_stack, &g_worker_tcb);
    if (g_worker == nullptr) {
        ESP_LOGE(kTag, "worker create failed");
        g_started.store(false);
        return;
    }
    ESP_LOGI(kTag, "kdraw watcher started");
}

}  // namespace kdraw
}  // namespace hutuji
