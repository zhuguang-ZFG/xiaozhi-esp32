#include "hutuji_job.h"

#include "hutuji_pipe.h"

#include "application.h"
#include "board.h"
#include "display.h"
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
// 暂停上限：超过则自动放弃，避免 busy_ 被无限期占用导致所有工具返回 busy。
constexpr uint32_t kMaxPauseMs = 10 * 60 * 1000;
} // namespace

Job& Job::GetInstance() {
    static Job instance;
    return instance;
}

void Job::SetStreamingOrPaused() {
    // 暂停中不得把状态写成 streaming，否则 hutuji.status 会谎报正在画。
    SetState(paused_.load() ? "paused" : "streaming");
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
    auto& pipe = Pipe::GetInstance();
    if (!pipe.IsConnected()) {
        busy_.store(false);
        return "{\"error\":\"写字机 Telnet 未连接\"}";
    }
    if (!pipe.IsReady()) {
        busy_.store(false);
        return "{\"error\":\"写字机未就绪（未收到版本应答）\"}";
    }
    abort_requested_.store(false);
    paper_active_.store(false);
    paused_.store(false);
    // 新任务：留存的旧 G-code 作废（Run 里 DownloadToPsram 会 ReleaseBuffer）
    repeat_mode_.store(false);
    buffer_replayable_.store(false);
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

std::string Job::RequestPause() {
    if (!busy_.load()) {
        return "{\"error\":\"当前没在出图\"}";
    }
    if (pen_test_active_.load()) {
        return "正在试笔，请稍候";
    }
    if (paper_active_.load()) {
        return "换纸中无法暂停，换纸完成后可再试";
    }
    // 与 SendLine 持同一把锁：`!` 发出后不能再有下一行进入 planner。
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
    if (paused_.exchange(true)) {
        return "已经是暂停状态";
    }
    // `!` 进给保持：Grbl 减速停住，planner 内容保留，`~` 可原地续跑。
    // 发送失败时回滚本地状态，不能把断链说成已暂停。
    if (!Pipe::GetInstance().SendRealtime('!')) {
        paused_.store(false);
        return "{\"error\":\"写字机 Telnet 已断开，无法暂停\"}";
    }
    SetState("paused");
    if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("已暂停");
    return "ok";
}

std::string Job::RequestResume() {
    if (!busy_.load()) {
        return "{\"error\":\"当前没在出图\"}";
    }
    // 与 RequestPause 对称：试笔期间两个工具给同一个解释。
    if (pen_test_active_.load()) {
        return "正在试笔，请稍候";
    }
    if (!paused_.exchange(false)) {
        return "本来就没暂停";
    }
    if (!Pipe::GetInstance().SendRealtime('~')) {
        paused_.store(true);
        return "{\"error\":\"写字机 Telnet 已断开，无法继续\"}";
    }
    SetState("streaming");
    if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("继续画...");
    return "ok";
}

std::string Job::RequestRepeat() {
    if (busy_.exchange(true)) {
        return "busy";
    }
    auto& pipe = Pipe::GetInstance();
    if (!pipe.IsConnected()) {
        busy_.store(false);
        return "{\"error\":\"写字机 Telnet 未连接\"}";
    }
    if (!pipe.IsReady()) {
        busy_.store(false);
        return "{\"error\":\"写字机未就绪（未收到版本应答）\"}";
    }
    // 有留存 buffer 走快路（跳下载+CRC）；否则回落重新下载上次 url_
    bool replay = buffer_replayable_.load() && buffer_ != nullptr && buffer_len_ > 0;
    if (!replay && url_.empty()) {
        busy_.store(false);
        return "{\"error\":\"还没画过东西，没有可重画的内容\"}";
    }
    abort_requested_.store(false);
    paused_.store(false);
    paper_active_.store(false);
    repeat_mode_.store(replay);
    last_error_.clear();
    SetState(replay ? "streaming" : "downloading");

    BaseType_t ok = xTaskCreate(TaskEntry, "hutuji_draw", 8192, this, 5, nullptr);
    if (ok != pdTRUE) {
        busy_.store(false);
        repeat_mode_.store(false);
        SetState("idle");
        return "{\"error\":\"无法创建出图任务\"}";
    }
    return replay ? "started" : "started_redownload";
}

std::string Job::RequestPenTest() {
    // 试笔也必须独占 Telnet 写入，避免与新出图的首行或重画交错。
    if (busy_.exchange(true)) {
        return "busy";
    }
    auto& pipe = Pipe::GetInstance();
    if (!pipe.IsConnected() || !pipe.IsReady()) {
        busy_.store(false);
        return "{\"error\":\"写字机未连接或未就绪\"}";
    }
    SetState("pen_test");
    pen_test_active_.store(true);
    // 不在 MCP 回调里 sleep：独立短任务做 M5 → M3 → 1s → M5
    BaseType_t created = xTaskCreate(
        [](void*) {
            auto& p = Pipe::GetInstance();
            auto& job = Job::GetInstance();
            bool ok = false;
            // 先抬笔建立确定的起点；随后一次明确的 M3 落笔必然可在纸上留下点迹。
            if (p.SendLine("M5") && p.WaitOk(3000) &&
                p.SendLine("M3 S1000") && p.WaitOk(3000)) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                ok = p.SendLine("M5") && p.WaitOk(3000);
            }
            job.Notify(ok ? "笔测试完成：已落笔停 1 秒再抬笔，请看纸上有没有点"
                          : "笔测试失败：写字机没正常应答");
            job.SetState(ok ? "done" : "error");
            job.pen_test_active_.store(false);
            job.busy_.store(false);
            vTaskDelete(nullptr);
        },
        "hutuji_pentest", 3072, nullptr, 5, nullptr);
    if (created != pdTRUE) {
        pen_test_active_.store(false);
        busy_.store(false);
        SetState("idle");
        return "{\"error\":\"无法创建试笔任务\"}";
    }
    return "started";
}

std::string Job::StatusJson() const {
    auto& pipe = Pipe::GetInstance();
    // 发 ? 查最新状态，等 150ms 让 PipeTask 收到并解析响应
    if (pipe.IsConnected()) {
        pipe.SendRealtime('?');
        vTaskDelay(pdMS_TO_TICKS(150));
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
    cJSON_AddBoolToObject(root, "repeat_available", buffer_replayable_.load());
    cJSON_AddStringToObject(root, "state", state.c_str());
    cJSON_AddStringToObject(root, "grbl_state", Pipe::GrblStateName(pipe.GetGrblState()));
    float mx, my, mz;
    pipe.GetMachinePos(mx, my, mz);
    cJSON_AddNumberToObject(root, "mpos_x", mx);
    cJSON_AddNumberToObject(root, "mpos_y", my);
    cJSON_AddNumberToObject(root, "mpos_z", mz);
    if (pipe.GetGrblState() == GrblState::Alarm) {
        cJSON_AddNumberToObject(root, "alarm_code", pipe.GetAlarmCode());
    }
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
        // 重画：buffer_ 里已是上次校验通过的内容，跳过下载与 CRC
        if (repeat_mode_.load()) {
            if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("重画中...");
            ESP_LOGI(TAG, "重画：复用 PSRAM %zu 字节，跳过下载/校验", buffer_len_);
        } else {
            SetState("downloading");
            if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("下载中...");
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
            if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("校验中...");
            if (!VerifyCrc()) {
                SetState("error");
                Notify(std::string("校验失败: ") + last_error_);
                break;
            }
        }
        auto& pipe = Pipe::GetInstance();
        if (!pipe.IsReady()) {
            last_error_ = "写字机未就绪";
            SetState("error");
            Notify(last_error_);
            break;
        }
        // 授权由 Grbl 端 check_license() 强制——未授权首条运动会返回 error:60
        // 下载/校验期间就被暂停时，不能把状态改回 streaming——否则 status 谎报
        // 正在画，实际转发循环一进去就卡在暂停门上。
        SetStreamingOrPaused();
        ok = StreamToGrbl();
        if (abort_requested_.load()) {
            SetState("aborted");
            if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("已取消");
        } else if (ok) {
            SetState("done");
            Notify("出图完成");
            if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("画好啦！");
        } else {
            SetState("error");
            Notify(std::string("转发失败: ") + last_error_);
            if (auto* d = Board::GetInstance().GetDisplay()) d->SetStatus("出错了");
        }
    } while (false);

    // 出图成功则留存 buffer_ 供 hutuji.repeat 复用（PSRAM 上限 512KB，只留一份）；
    // 失败/中止时释放，避免重画一份画坏的内容。
    if (ok && !abort_requested_.load() && buffer_ != nullptr) {
        buffer_replayable_.store(true);
    } else {
        buffer_replayable_.store(false);
        ReleaseBuffer();
    }
    paper_active_.store(false);
    paused_.store(false);
    repeat_mode_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ESP_LOGI(TAG, "任务结束 state=%s err=%s replayable=%d",
                 state_.c_str(), last_error_.c_str(), (int)buffer_replayable_.load());
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

size_t Job::CountLines() const {
    size_t count = 0;
    size_t i = 0;
    while (i < buffer_len_) {
        size_t start = i;
        while (i < buffer_len_ && buffer_[i] != '\n' && buffer_[i] != '\r') ++i;
        if (i > start) ++count;
        while (i < buffer_len_ && (buffer_[i] == '\n' || buffer_[i] == '\r')) ++i;
    }
    return count;
}

void Job::UpdateDisplayProgress() {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display) return;

    char buf[48];
    if (paper_active_.load()) {
        snprintf(buf, sizeof(buf), "换纸中... %zu/%zu", lines_sent_, lines_total_);
    } else {
        int pct = lines_total_ > 0
            ? static_cast<int>(lines_sent_ * 100 / lines_total_) : 0;
        snprintf(buf, sizeof(buf), "画画中 %d%%  (%zu/%zu)", pct, lines_sent_, lines_total_);
    }
    display->SetStatus(buf);
}

bool Job::WaitWhilePaused() {
    if (!paused_.load()) {
        return true;
    }
    auto& pipe = Pipe::GetInstance();
    TickType_t began = xTaskGetTickCount();
    while (paused_.load() && !abort_requested_.load()) {
        if (!pipe.IsConnected() || !pipe.IsReady()) {
            last_error_ = "暂停中链路丢失";
            return false;
        }
        // 暂停不能无限期挂住 busy_，否则 draw/repeat/pen_test 全被顶成 busy，
        // 用户只剩 abort 一条出路。超时按放弃处理，并如实告知云端。
        if ((xTaskGetTickCount() - began) >= pdMS_TO_TICKS(kMaxPauseMs)) {
            paused_.store(false);
            abort_requested_.store(true);
            last_error_ = "暂停超时自动取消";
            Notify("暂停超过 10 分钟，已自动取消这幅画；想画的话跟我说一声");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return !abort_requested_.load();
}

bool Job::StreamToGrbl() {
    auto& pipe = Pipe::GetInstance();

    lines_total_ = CountLines();
    lines_sent_ = 0;
    UpdateDisplayProgress();
    TickType_t last_notify_tick = xTaskGetTickCount();

    size_t i = 0;
    while (i < buffer_len_) {
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        // 暂停门：停在行边界，不切断已发出的行。Grbl 侧由 `!` 做进给保持，
        // 这里只是不再灌新行，避免暂停期间 planner 继续被填满。
        if (!WaitWhilePaused()) {
            if (last_error_.empty()) last_error_ = "aborted";
            return false;
        }
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        if (!pipe.IsConnected() || !pipe.IsReady()) {
            last_error_ = "转发中链路丢失";
            return false;
        }
        if (pipe.GetGrblState() == GrblState::Alarm) {
            last_error_ = "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
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
            ++lines_sent_;
            continue;
        }

        bool paper_line = LooksLikePaperLine(line);
        if (paper_line) {
            paper_active_.store(true);
            SetState("paper_change");
        }

        int retries = 0;
        while (true) {
            // 行已经解析但尚未写入时也可能收到 pause；在这里等待而不是忙等重试。
            if (!WaitWhilePaused()) {
                if (last_error_.empty()) last_error_ = "aborted";
                return false;
            }
            if (abort_requested_.load() && !paper_active_.load()) {
                last_error_ = "aborted";
                return false;
            }
            {
                // 与 RequestPause() 同步：暂停命令发出后，本行不能再进入 planner。
                std::lock_guard<std::mutex> stream_lock(stream_mutex_);
                if (paused_.load()) {
                    continue;
                }
                if (!pipe.SendLine(line)) {
                    last_error_ = "SendLine 失败";
                    return false;
                }
            }

            uint32_t timeout = paper_line ? kPaperOkTimeoutMs
                                          : (LooksLikeMotionLine(line) ? kMotionOkTimeoutMs : kOkTimeoutMs);
            // 分段等，便于响应 abort
            WaitResult wr = WaitResult::Timeout;
            int err = -1;
            uint32_t waited = 0;
            const uint32_t slice = 1000;
            while (waited < timeout) {
                // 当前行已发送时也要冻结等待超时；否则暂停超过 timeout 会被误判为转发失败。
                if (!WaitWhilePaused()) {
                    if (last_error_.empty()) last_error_ = "aborted";
                    return false;
                }
                if (abort_requested_.load() && !paper_active_.load()) {
                    last_error_ = "aborted";
                    return false;
                }
                if (pipe.GetGrblState() == GrblState::Alarm) {
                    last_error_ = "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
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
                ++lines_sent_;
                if (paper_line) {
                    paper_active_.store(false);
                    // 换纸期间用户可能已按暂停：别把 paused 覆盖成 streaming。
                    SetState(paused_.load() ? "paused" : "streaming");
                    if (abort_requested_.load()) {
                        last_error_ = "aborted";
                        return false;
                    }
                }
                UpdateDisplayProgress();
                // 每 5s 向云端推送一次进度
                TickType_t now = xTaskGetTickCount();
                if ((now - last_notify_tick) >= pdMS_TO_TICKS(5000)) {
                    last_notify_tick = now;
                    float mx, my, mz;
                    pipe.GetMachinePos(mx, my, mz);
                    int pct = lines_total_ > 0
                        ? static_cast<int>(lines_sent_ * 100 / lines_total_) : 0;
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "出图进度: %d%% (%zu/%zu行) 位置 X=%.1f Y=%.1f",
                             pct, lines_sent_, lines_total_, mx, my);
                    Notify(buf);
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
