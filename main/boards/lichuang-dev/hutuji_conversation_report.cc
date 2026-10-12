#include "hutuji_conversation_report.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "http.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>

namespace hutuji {
namespace {

constexpr const char* kTag = "HutujiConvRpt";
constexpr const char* kReportUrl =
    "https://hutuji.donglicao.com/draw-upload/api/device/conversation/report";
// 静态栈在链接期预留，避免对话高峰期 heap 凑不出 12KiB 连续块
// （COM14：worker create failed）。HTTPS/TLS 峰值仍需 ≥12KiB。
constexpr uint32_t kTaskStackWords = 12288 / sizeof(StackType_t);
constexpr UBaseType_t kTaskPriority = 1;  // 低于音频/网络主路径
constexpr size_t kMaxContent = 500;
constexpr size_t kMaxSession = 64;
constexpr size_t kQueueCap = 8;
constexpr TickType_t kIdlePollTicks = pdMS_TO_TICKS(500);

struct PendingTurn {
    char role[16];
    char session_id[kMaxSession];
    char content[kMaxContent + 1];
    double client_ts;
};

SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_wake = nullptr;
PendingTurn g_queue[kQueueCap];
size_t g_head = 0;
size_t g_count = 0;
TaskHandle_t g_worker = nullptr;
StackType_t g_worker_stack[kTaskStackWords];
StaticTask_t g_worker_tcb;
// 冲刷缓冲放静态区，避免 worker 栈再叠 ~4.7KiB。
PendingTurn g_flush_batch[kQueueCap];

bool PushTurn(const char* role, const char* content, const std::string& session_id) {
    if (g_mutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (g_count >= kQueueCap) {
        g_head = (g_head + 1) % kQueueCap;
        g_count--;
    }
    size_t idx = (g_head + g_count) % kQueueCap;
    PendingTurn& slot = g_queue[idx];
    std::strncpy(slot.role, role ? role : "user", sizeof(slot.role) - 1);
    slot.role[sizeof(slot.role) - 1] = '\0';
    std::strncpy(slot.session_id, session_id.c_str(), sizeof(slot.session_id) - 1);
    slot.session_id[sizeof(slot.session_id) - 1] = '\0';
    size_t len = content ? std::strlen(content) : 0;
    if (len > kMaxContent) {
        len = kMaxContent;
    }
    if (len > 0 && content != nullptr) {
        std::memcpy(slot.content, content, len);
    }
    slot.content[len] = '\0';
    slot.client_ts = static_cast<double>(esp_timer_get_time()) / 1e6;
    g_count++;
    xSemaphoreGive(g_mutex);
    return true;
}

size_t PeekCount() {
    if (g_mutex == nullptr) {
        return 0;
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    size_t n = g_count;
    xSemaphoreGive(g_mutex);
    return n;
}

size_t PopBatch(PendingTurn* out, size_t max_out) {
    if (g_mutex == nullptr || out == nullptr || max_out == 0) {
        return 0;
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    size_t n = g_count < max_out ? g_count : max_out;
    for (size_t i = 0; i < n; ++i) {
        out[i] = g_queue[(g_head + i) % kQueueCap];
    }
    g_head = (g_head + n) % kQueueCap;
    g_count -= n;
    xSemaphoreGive(g_mutex);
    return n;
}

bool DeviceIsIdleForFlush() {
    return Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
}

void PostBatch(const PendingTurn* turns, size_t count) {
    if (turns == nullptr || count == 0) {
        return;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(kTag, "no network, drop batch=%u", (unsigned)count);
        return;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "session_id", turns[0].session_id);
    cJSON* events = cJSON_CreateArray();
    if (events == nullptr) {
        cJSON_Delete(root);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON* ev = cJSON_CreateObject();
        if (ev == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(ev, "role", turns[i].role);
        cJSON_AddStringToObject(ev, "content", turns[i].content);
        cJSON_AddNumberToObject(ev, "client_ts", turns[i].client_ts);
        cJSON_AddItemToArray(events, ev);
    }
    cJSON_AddItemToObject(root, "events", events);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return;
    }

    auto http = network->CreateHttp(5);
    if (http == nullptr) {
        ESP_LOGW(kTag, "CreateHttp failed");
        cJSON_free(body);
        return;
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(body));
    cJSON_free(body);
    if (!http->Open("POST", kReportUrl)) {
        ESP_LOGW(kTag, "report open failed");
    } else {
        ESP_LOGI(kTag, "report status=%d batch=%u (idle flush)", http->GetStatusCode(),
                 (unsigned)count);
        http->Close();
    }
}

void WorkerTask(void* /*arg*/) {
    for (;;) {
        xSemaphoreTake(g_wake, kIdlePollTicks);
        if (PeekCount() == 0) {
            continue;
        }
        if (!DeviceIsIdleForFlush()) {
            // 说话/聆听中只排队，绝不发 HTTPS。
            continue;
        }
        size_t n = PopBatch(g_flush_batch, kQueueCap);
        if (n > 0) {
            PostBatch(g_flush_batch, n);
        }
    }
}

void EnsureWorkerStarted() {
    if (g_worker != nullptr) {
        return;
    }
    g_worker = xTaskCreateStatic(WorkerTask, "hutuji_conv_rpt", kTaskStackWords, nullptr,
                                 kTaskPriority, g_worker_stack, &g_worker_tcb);
    if (g_worker == nullptr) {
        ESP_LOGE(kTag, "static worker create failed");
    }
}

void EnsureInit() {
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    if (g_wake == nullptr) {
        g_wake = xSemaphoreCreateBinary();
    }
}

}  // namespace

void InitConversationReport() {
    EnsureInit();
    EnsureWorkerStarted();
}

void ReportConversationTurn(const char* role, const char* content,
                            const std::string& session_id) {
    if (role == nullptr || content == nullptr || session_id.empty()) {
        return;
    }
    if (content[0] == '\0') {
        return;
    }
    EnsureInit();
    if (g_mutex == nullptr || g_worker == nullptr) {
        return;
    }
    if (!PushTurn(role, content, session_id)) {
        ESP_LOGW(kTag, "queue push failed, drop turn");
        return;
    }
    if (g_wake != nullptr) {
        xSemaphoreGive(g_wake);
    }
}

}  // namespace hutuji
