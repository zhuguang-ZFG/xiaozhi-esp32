#include "hutuji_job.h"

#include "hutuji_pipe.h"

#include "application.h"
#include "board.h"
#include "http.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <cstdlib>
#include <cstring>


#define TAG "HutujiJob"

namespace hutuji {

namespace {
constexpr size_t kMaxGcodeBytes = 512 * 1024;
constexpr uint32_t kOkTimeoutMs = 60000;
constexpr uint32_t kPaperOkTimeoutMs = 90000;
constexpr uint32_t kMotionOkTimeoutMs = 30000;
constexpr int kDeferredMaxRetries = 8;
} // namespace

Job& Job::GetInstance() {
    static Job instance;
    return instance;
}

void Job::SetState(const char* state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = state;
}

void Job::Notify(const std::string& message) {
    // 设备侧 MCP 主动推送（云端可见）；失败仅打日志
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

void Job::ReleaseBuffer() {
    if (buffer_ != nullptr) {
        heap_caps_free(buffer_);
        buffer_ = nullptr;
    }
    buffer_len_ = 0;
    have_crc_ = false;
}

uint32_t Job::Crc32Ieee(const uint8_t* data, size_t len) {
    // zlib/IEEE CRC32，与 Python zlib.crc32 一致（初值 0，结果已按惯例取反折叠）
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool Job::LooksLikePaperLine(const std::string& line) {
    // 粗匹配现役换纸相关；不解析参数语义
    if (line.rfind("M30", 0) == 0) return true;
    if (line.rfind("M721", 0) == 0) return true;
    if (line.rfind("M701", 0) == 0) return true;
    if (line.find("[ESP910]") != std::string::npos) return true;
    return false;
}

bool Job::LooksLikeMotionLine(const std::string& line) {
    if (line.empty()) return false;
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
    if (c != 'G') return false;
    // G0–G3
    if (line.size() >= 2 && line[1] >= '0' && line[1] <= '3') {
        if (line.size() == 2 || !std::isdigit(static_cast<unsigned char>(line[2]))) {
            return true;
        }
    }
    return false;
}

std::string Job::StartDraw(const std::string& url) {
    if (url.empty()) {
        return "{\"error\":\"url 不能为空\"}";
    }
    if (busy_.exchange(true)) {
        return "busy";
    }
    abort_requested_.store(false);
    paper_active_.store(false);
    url_ = url;
    last_error_.clear();
    SetState("downloading");

    BaseType_t ok = xTaskCreate(TaskEntry, "hutuji_draw", 8192, this, 5, nullptr);
    if (ok != pdTRUE) {
        busy_.store(false);
        SetState("idle");
        return "{\"error\":\"无法创建出图任务\"}";
    }
    return "started";
}

std::string Job::RequestAbort() {
    if (!busy_.load()) {
        return "ok";
    }
    abort_requested_.store(true);
    if (paper_active_.load()) {
        Notify("写字机正在换纸，无法即停；换纸完成后任务将停止");
        return "换纸中无法即停，完成后即停";
    }
    // 不在 MCP 回调里 sleep：独立短任务发 ! → 8s → 0x18
    xTaskCreate(
        [](void*) {
            auto& pipe = Pipe::GetInstance();
            auto& job = Job::GetInstance();
            pipe.SendRealtime('!');
            vTaskDelay(pdMS_TO_TICKS(8000));
            if (!job.IsPaperActive()) {
                pipe.SendRealtime(static_cast<char>(0x18));
            }
            vTaskDelete(nullptr);
        },
        "hutuji_abort", 2048, nullptr, 6, nullptr);
    return "ok";
}

std::string Job::StatusJson() const {
    auto& pipe = Pipe::GetInstance();
    // 出图中只发 ?；空闲可发探活类命令——此处仅 ?
    if (busy_.load() && pipe.IsConnected()) {
        pipe.SendRealtime('?');
    }

    std::string state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state = state_;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", pipe.IsConnected());
    cJSON_AddBoolToObject(root, "ready", pipe.IsReady());
    cJSON_AddBoolToObject(root, "authorized", pipe.IsAuthorized());
    cJSON_AddStringToObject(root, "state", state.c_str());
    cJSON_AddStringToObject(root, "last_line", pipe.GetLastLine().c_str());
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string json = str ? str : "{}";
    cJSON_free(str);
    return json;
}

void Job::TaskEntry(void* arg) {
    static_cast<Job*>(arg)->Run();
    vTaskDelete(nullptr);
}

void Job::Run() {
    bool ok = false;
    do {
        if (abort_requested_.load()) {
            SetState("aborted");
            break;
        }
        SetState("downloading");
        if (!DownloadToPsram(url_)) {
            SetState("error");
            Notify(std::string("下载失败: ") + last_error_);
            break;
        }
        if (abort_requested_.load()) {
            SetState("aborted");
            break;
        }
        SetState("verifying");
        if (!VerifyCrc()) {
            SetState("error");
            Notify(std::string("校验失败: ") + last_error_);
            break;
        }
        auto& pipe = Pipe::GetInstance();
        if (!pipe.IsReady()) {
            last_error_ = "写字机未就绪";
            SetState("error");
            Notify(last_error_);
            break;
        }
        if (!pipe.IsAuthorized()) {
            last_error_ = "未授权，零字节下发；请先 M800 授权";
            SetState("error");
            Notify(last_error_);
            break;
        }
        SetState("streaming");
        ok = StreamToGrbl();
        if (abort_requested_.load()) {
            SetState("aborted");
        } else if (ok) {
            SetState("done");
            Notify("出图完成");
        } else {
            SetState("error");
            Notify(std::string("转发失败: ") + last_error_);
        }
    } while (false);

    ReleaseBuffer();
    paper_active_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ESP_LOGI(TAG, "任务结束 state=%s err=%s", state_.c_str(), last_error_.c_str());
    }
    busy_.store(false);
}

bool Job::DownloadToPsram(const std::string& url) {
    ReleaseBuffer();
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        last_error_ = "无网络";
        return false;
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        last_error_ = "CreateHttp 失败";
        return false;
    }
    http->SetTimeout(60000);
    // 自签证书在正式证换上前可能失败；联调可用 http:// PUBLIC_BASE_URL
    if (!http->Open("GET", url)) {
        last_error_ = "HTTP Open 失败";
        return false;
    }
    if (http->GetStatusCode() != 200) {
        last_error_ = "HTTP status " + std::to_string(http->GetStatusCode());
        http->Close();
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        last_error_ = "Content-Length 为 0";
        http->Close();
        return false;
    }
    if (content_length > kMaxGcodeBytes) {
        last_error_ = "超过 512KB";
        http->Close();
        return false;
    }

    std::string crc_hdr = http->GetResponseHeader("X-Hutuji-CRC32");
    if (crc_hdr.empty()) {
        crc_hdr = http->GetResponseHeader("x-hutuji-crc32");
    }
    if (!crc_hdr.empty()) {
        expect_crc_ = static_cast<uint32_t>(strtoul(crc_hdr.c_str(), nullptr, 16));
        have_crc_ = true;
    } else {
        last_error_ = "缺少 X-Hutuji-CRC32";
        http->Close();
        return false;
    }

    buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer_ == nullptr) {
        buffer_ = static_cast<uint8_t*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT));
    }
    if (buffer_ == nullptr) {
        last_error_ = "PSRAM 分配失败";
        http->Close();
        return false;
    }

    size_t total = 0;
    while (total < content_length) {
        if (abort_requested_.load()) {
            http->Close();
            last_error_ = "aborted";
            return false;
        }
        int n = http->Read(reinterpret_cast<char*>(buffer_ + total), content_length - total);
        if (n < 0) {
            last_error_ = "HTTP Read 失败";
            http->Close();
            return false;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<size_t>(n);
    }
    http->Close();

    if (total != content_length) {
        last_error_ = "长度不符 expect=" + std::to_string(content_length) +
                      " got=" + std::to_string(total);
        return false;
    }
    buffer_len_ = total;
    ESP_LOGI(TAG, "下载完成 %u 字节 crc_hdr=%08x", (unsigned)buffer_len_, (unsigned)expect_crc_);
    return true;
}

bool Job::VerifyCrc() {
    uint32_t got = Crc32Ieee(buffer_, buffer_len_);
    if (!have_crc_ || got != expect_crc_) {
        last_error_ = "CRC 不符";
        ESP_LOGE(TAG, "CRC expect=%08x got=%08x", (unsigned)expect_crc_, (unsigned)got);
        return false;
    }
    return true;
}

bool Job::StreamToGrbl() {
    auto& pipe = Pipe::GetInstance();
    // 按行切分（保留在 PSRAM，不复制整份到 vector 字符串过多时可流式扫）
    size_t i = 0;
    while (i < buffer_len_) {
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        if (!pipe.IsConnected() || !pipe.IsReady()) {
            last_error_ = "转发中链路丢失";
            return false;
        }

        size_t start = i;
        while (i < buffer_len_ && buffer_[i] != '\n' && buffer_[i] != '\r') {
            ++i;
        }
        std::string line(reinterpret_cast<char*>(buffer_ + start), i - start);
        while (i < buffer_len_ && (buffer_[i] == '\n' || buffer_[i] == '\r')) {
            ++i;
        }
        // 去注释与空行
        auto hash = line.find(';');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) {
            ++b;
        }
        if (b > 0) {
            line.erase(0, b);
        }
        if (line.empty()) {
            continue;
        }

        bool paper_line = LooksLikePaperLine(line);
        if (paper_line) {
            paper_active_.store(true);
            SetState("paper_change");
        }

        int retries = 0;
        while (true) {
            if (abort_requested_.load() && !paper_active_.load()) {
                last_error_ = "aborted";
                return false;
            }
            if (!pipe.SendLine(line)) {
                last_error_ = "SendLine 失败";
                return false;
            }

            uint32_t timeout = paper_line ? kPaperOkTimeoutMs
                                          : (LooksLikeMotionLine(line) ? kMotionOkTimeoutMs : kOkTimeoutMs);
            // 分段等，便于响应 abort
            WaitResult wr = WaitResult::Timeout;
            int err = -1;
            uint32_t waited = 0;
            const uint32_t slice = 1000;
            while (waited < timeout) {
                if (abort_requested_.load() && !paper_active_.load()) {
                    last_error_ = "aborted";
                    return false;
                }
                uint32_t step = (timeout - waited > slice) ? slice : (timeout - waited);
                wr = pipe.WaitResponse(step, nullptr, &err);
                if (wr != WaitResult::Timeout) {
                    break;
                }
                waited += step;
            }

            if (wr == WaitResult::Ok) {
                if (paper_line) {
                    paper_active_.store(false);
                    SetState("streaming");
                    if (abort_requested_.load()) {
                        last_error_ = "aborted";
                        return false;
                    }
                }
                break;
            }
            if (wr == WaitResult::Deferred) {
                // error:8：暂停后重发本行
                paper_active_.store(true);
                SetState("paper_change");
                ++retries;
                if (retries > kDeferredMaxRetries) {
                    last_error_ = "error:8 重试耗尽";
                    return false;
                }
                ESP_LOGW(TAG, "error:8，%d 次重发: %s", retries, line.c_str());
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (wr == WaitResult::Failed) {
                last_error_ = "error:" + std::to_string(err);
                return false;
            }
            // Timeout：发 ? 探活一次再判失败
            pipe.SendRealtime('?');
            last_error_ = "等 ok 超时";
            return false;
        }
    }
    return true;
}

} // namespace hutuji
