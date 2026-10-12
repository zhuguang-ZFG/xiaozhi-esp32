#include "hutuji_activation_relay.h"

#include "board.h"
#include "http.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

namespace hutuji {
namespace {

constexpr const char* kTag = "HutujiActRelay";
// 京东云生产入口（Cloudflare Tunnel → nginx → uvicorn）。与 G-code 下载同一
// 公网面，S3 板对该域名的 HTTPS 链路已有生产证据（hutuji_job 下载同域）。
constexpr const char* kRelayUrl = "https://hutuji.donglicao.com/register";
// 一次性上报任务栈：HTTPS(TLS 握手) + JSON，8K 与下载路径同级；激活码有效期
// 以分钟计，异步几秒内完成不影响无感体验。
constexpr uint32_t kRelayTaskStack = 8192;

struct RelayPayload {
    char code[12];
};

bool IsPlausibleCode(const std::string& code) {
    // 服务端口径 4~8 位数字；不合法的输入不值得发请求。
    if (code.size() < 4 || code.size() > 8) {
        return false;
    }
    for (char c : code) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

void RelayTask(void* arg) {
    auto* payload = static_cast<RelayPayload*>(arg);
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(kTag, "no network, skip relay");
        delete payload;
        vTaskDelete(nullptr);
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "code", payload->code);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        delete payload;
        vTaskDelete(nullptr);
        return;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        ESP_LOGW(kTag, "CreateHttp failed, skip relay");
        cJSON_free(body);
        delete payload;
        vTaskDelete(nullptr);
        return;
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(body));
    cJSON_free(body);
    // 脱敏硬约定：日志只记 HTTP 状态与 MAC，绝不记激活码与请求体。
    if (!http->Open("POST", kRelayUrl)) {
        ESP_LOGW(kTag, "relay open failed mac=%s", SystemInfo::GetMacAddress().c_str());
    } else {
        const int status = http->GetStatusCode();
        ESP_LOGI(kTag, "relay mac=%s status=%d", SystemInfo::GetMacAddress().c_str(), status);
        http->Close();
    }
    delete payload;
    vTaskDelete(nullptr);
}

}  // namespace

void ReportActivationCode(const std::string& code) {
    if (!IsPlausibleCode(code)) {
        ESP_LOGW(kTag, "implausible code length=%u, skip relay", (unsigned)code.size());
        return;
    }
    auto* payload = new RelayPayload{};
    // code 已验 4~8 位数字，截断拷贝安全。
    std::strncpy(payload->code, code.c_str(), sizeof(payload->code) - 1);
    if (xTaskCreate(RelayTask, "hutuji_act_relay", kRelayTaskStack, payload, 3, nullptr) !=
        pdPASS) {
        ESP_LOGW(kTag, "relay task create failed, skip");
        delete payload;
    }
}

}  // namespace hutuji
