#include "hutuji_ota.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "display.h"
#include "http.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#include "boards/lichuang-dev/hutuji_job.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace hutuji::ota {
namespace {

constexpr const char* kTag = "HutujiOta";
constexpr const char* kDefaultPublicBase = "https://hutuji.donglicao.com";
constexpr const char* kDefaultAllowedHost = "hutuji.donglicao.com";
constexpr const char* kNvsNs = "hutuji_ota";
constexpr const char* kNvsLastDay = "last_day";
constexpr const char* kNvsPublicBase = "public_base";
constexpr const char* kNvsAllowedHosts = "allowed_hosts";

std::atomic<bool> g_check_inflight{false};

std::string JsonEscaped(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string MakeCheckJson(bool update_available, const std::string& current,
                          const std::string& latest, const std::string& notes,
                          const std::string& reason) {
    std::string json = "{\"update_available\":";
    json += update_available ? "true" : "false";
    json += ",\"current\":\"";
    json += JsonEscaped(current);
    json += "\",\"latest\":\"";
    json += JsonEscaped(latest);
    json += "\",\"notes\":\"";
    json += JsonEscaped(notes);
    json += "\",\"reason\":\"";
    json += JsonEscaped(reason);
    json += "\"}";
    return json;
}

std::string MakeReasonJson(const std::string& reason) {
    return std::string("{\"reason\":\"") + JsonEscaped(reason) + "\"}";
}

std::string CurrentFirmwareVersion() {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc != nullptr && app_desc->version[0] != '\0') {
        return app_desc->version;
    }
    return "";
}

std::string PublicBaseUrl() {
    Settings settings(kNvsNs, false);
    std::string base = settings.GetString(kNvsPublicBase);
    if (base.empty()) {
        base = kDefaultPublicBase;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

std::vector<int> ParseVersionDigits(const std::string& version) {
    // 与 Ota::ParseVersion 同口径：跳过非纯数字段（如 "hutuji"）。
    std::vector<int> numbers;
    std::stringstream ss(version);
    std::string segment;
    while (std::getline(ss, segment, '.')) {
        if (segment.empty()) {
            continue;
        }
        bool all_digit = std::all_of(segment.begin(), segment.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
        if (!all_digit) {
            continue;
        }
        try {
            numbers.push_back(std::stoi(segment));
        } catch (...) {
            continue;
        }
    }
    return numbers;
}

bool IsNewerVersion(const std::string& current, const std::string& newer) {
    std::vector<int> cur = ParseVersionDigits(current);
    std::vector<int> neu = ParseVersionDigits(newer);
    for (size_t i = 0; i < std::min(cur.size(), neu.size()); ++i) {
        if (neu[i] > cur[i]) {
            return true;
        }
        if (neu[i] < cur[i]) {
            return false;
        }
    }
    return neu.size() > cur.size();
}

bool ExtractHttpsHost(const std::string& url, std::string& host_out) {
    host_out.clear();
    constexpr const char* kPrefix = "https://";
    if (url.size() < 8 || url.compare(0, 8, kPrefix) != 0) {
        return false;
    }
    size_t host_start = 8;
    size_t host_end = url.find_first_of("/:?#", host_start);
    if (host_end == std::string::npos) {
        host_end = url.size();
    }
    if (host_end <= host_start) {
        return false;
    }
    host_out = url.substr(host_start, host_end - host_start);
    return !host_out.empty();
}

bool HostAllowed(const std::string& url) {
    std::string host;
    if (!ExtractHttpsHost(url, host)) {
        return false;
    }
    // 默认仅 hutuji.donglicao.com；NVS allowed_hosts 可扩（含显式 IP）。
    // 未列入则拒——含任意 IP 字面量。
    Settings settings(kNvsNs, false);
    std::string allow = settings.GetString(kNvsAllowedHosts);
    if (allow.empty()) {
        allow = kDefaultAllowedHost;
    }
    std::string token;
    std::stringstream ss(allow);
    while (std::getline(ss, token, ',')) {
        size_t a = token.find_first_not_of(" \t");
        size_t b = token.find_last_not_of(" \t");
        if (a == std::string::npos) {
            continue;
        }
        token = token.substr(a, b - a + 1);
        if (token == host) {
            return true;
        }
    }
    return false;
}

bool JobIsIdleForOta() {
    // 与 portal assert_upgrade_allowed 对齐：仅 state==idle。
    cJSON* root = cJSON_Parse(Job::GetInstance().StatusJson().c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* state = cJSON_GetObjectItem(root, "state");
    const bool idle = cJSON_IsString(state) && state->valuestring != nullptr &&
                      std::strcmp(state->valuestring, "idle") == 0;
    cJSON_Delete(root);
    return idle;
}

std::string TodayUtcYmd() {
    time_t now = time(nullptr);
    struct tm tm_utc = {};
    gmtime_r(&now, &tm_utc);
    const int year = tm_utc.tm_year + 1900;
    const int mon = tm_utc.tm_mon + 1;
    const int day = tm_utc.tm_mday;
    // 钳位避免 -Werror=format-truncation 把 tm 字段当成任意 int。
    const unsigned y = year > 0 ? static_cast<unsigned>(year) : 0u;
    const unsigned m = (mon >= 1 && mon <= 12) ? static_cast<unsigned>(mon) : 1u;
    const unsigned d = (day >= 1 && day <= 31) ? static_cast<unsigned>(day) : 1u;
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", y, m, d);
    return buf;
}

struct CheckResult {
    bool ok = false;
    bool update_available = false;
    std::string current;
    std::string latest;
    std::string notes;
    std::string reason;
};

CheckResult RunOtaCheck(bool persist_day) {
    CheckResult r;
    r.current = CurrentFirmwareVersion();

    if (!WifiManager::GetInstance().IsConnected()) {
        r.reason = "check_failed";
        return r;
    }

    const std::string url =
        PublicBaseUrl() + "/firmware/s3/" + BOARD_NAME + "/latest.json";
    ESP_LOGI(kTag, "ota_check GET %s", url.c_str());

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        r.reason = "check_failed";
        return r;
    }
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        r.reason = "check_failed";
        return r;
    }
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", url)) {
        ESP_LOGW(kTag, "ota_check open failed err=0x%x", http->GetLastError());
        r.reason = "check_failed";
        return r;
    }
    const int status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(kTag, "ota_check HTTP %d", status);
        r.reason = "check_failed";
        return r;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        r.reason = "check_failed";
        return r;
    }
    cJSON* version = cJSON_GetObjectItem(root, "version");
    cJSON* notes = cJSON_GetObjectItem(root, "notes");
    cJSON* board = cJSON_GetObjectItem(root, "board");
    if (!cJSON_IsString(version) || version->valuestring == nullptr ||
        version->valuestring[0] == '\0') {
        cJSON_Delete(root);
        r.reason = "check_failed";
        return r;
    }
    if (cJSON_IsString(board) && board->valuestring != nullptr &&
        std::strcmp(board->valuestring, BOARD_NAME) != 0) {
        cJSON_Delete(root);
        r.reason = "board_mismatch";
        Job::GetInstance().SetOtaUpdateAvailable(false);
        return r;
    }
    r.latest = version->valuestring;
    if (cJSON_IsString(notes) && notes->valuestring != nullptr) {
        r.notes = notes->valuestring;
    }
    cJSON_Delete(root);

    r.update_available = IsNewerVersion(r.current, r.latest);
    Job::GetInstance().SetOtaUpdateAvailable(r.update_available);
    r.ok = true;
    r.reason = r.update_available ? "update_available" : "up_to_date";

    if (persist_day) {
        Settings settings(kNvsNs, true);
        settings.SetString(kNvsLastDay, TodayUtcYmd());
    }
    return r;
}

void HintUpdateAvailable() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowNotification("有固件更新", 5000);
    }
}

}  // namespace

void RegisterTools(McpServer& mcp_server) {
    mcp_server.AddTool(
        "hutuji.ota_check",
        "检查本机小派固件是否有更新：只查询最新版本清单，不下载、不升级。"
        "返回 JSON：update_available/current/latest/notes/reason。"
        "用户说「有新固件吗/检查更新」时用；真正升级走 hutuji.ota_start。",
        PropertyList(), [](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            auto r = RunOtaCheck(/*persist_day=*/false);
            return MakeCheckJson(r.update_available, r.current, r.latest, r.notes, r.reason);
        });

    mcp_server.AddTool(
        "hutuji.ota_start",
        "开始从已签发的 HTTPS 地址升级本机固件。"
        "参数 url/version/board 必填，sha256 可选。"
        "仅 idle 且 board 匹配、URL host 在允许列表时受理；出图中会返回 reason=busy。"
        "成功受理后设备进入升级并可能重启；失败则恢复运行并回 status.ota.state=failed。",
        PropertyList({Property("url", kPropertyTypeString), Property("version", kPropertyTypeString),
                      Property("board", kPropertyTypeString),
                      Property("sha256", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string& url = properties["url"].value<std::string>();
            const std::string& version = properties["version"].value<std::string>();
            const std::string& board = properties["board"].value<std::string>();
            const std::string& sha256 = properties["sha256"].value<std::string>();
            (void)sha256;  // 本批 Ota::Upgrade 无 sha 校验；门户已核包，参数仅契约对齐

            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateUpgrading || !JobIsIdleForOta()) {
                return MakeReasonJson("busy");
            }
            if (board != BOARD_NAME) {
                return MakeReasonJson("board_mismatch");
            }
            if (!HostAllowed(url)) {
                return MakeReasonJson("check_failed");
            }
            if (version.empty()) {
                return MakeReasonJson("check_failed");
            }

            Job::GetInstance().SetOtaStatus("upgrading", 0, "");
            Job::GetInstance().SetOtaUpdateAvailable(true);

            app.Schedule([url, version]() {
                auto& application = Application::GetInstance();
                const bool ok = application.UpgradeFirmware(url, version);
                if (!ok) {
                    Job::GetInstance().SetOtaStatus("failed", 0, "download_failed");
                    ESP_LOGE(kTag, "ota_start UpgradeFirmware failed");
                }
                // 成功路径会 reboot，无需清状态。
            });
            return std::string("{\"ok\":true}");
        });
}

void MaybeDailyCheck() {
    if (g_check_inflight.load()) {
        return;
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        return;
    }
    if (!JobIsIdleForOta()) {
        return;
    }

    const std::string today = TodayUtcYmd();
    {
        Settings settings(kNvsNs, false);
        if (settings.GetString(kNvsLastDay) == today) {
            return;
        }
    }

    bool expected = false;
    if (!g_check_inflight.compare_exchange_strong(expected, true)) {
        return;
    }

    // HTTP 不阻塞主循环：独立任务跑检查。
    // 日历日在任务启动时即写入，避免失败时每分钟重锤公网；次日再试。
    BaseType_t created = xTaskCreate(
        [](void* /*arg*/) {
            {
                Settings settings(kNvsNs, true);
                settings.SetString(kNvsLastDay, TodayUtcYmd());
            }
            auto r = RunOtaCheck(/*persist_day=*/false);
            if (r.ok && r.update_available) {
                HintUpdateAvailable();
            } else if (!r.ok) {
                ESP_LOGD(kTag, "daily ota_check skip/fail reason=%s", r.reason.c_str());
            }
            g_check_inflight.store(false);
            vTaskDelete(nullptr);
        },
        "hutuji_ota_chk", 8192, nullptr, 3, nullptr);
    if (created != pdPASS) {
        g_check_inflight.store(false);
        ESP_LOGW(kTag, "daily check task create failed");
    }
}

}  // namespace hutuji::ota
