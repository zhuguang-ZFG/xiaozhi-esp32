#include "hutuji_job.h"

#include "hutuji_pipe.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "http.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cJSON.h>
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
constexpr uint32_t kJobIdleTimeoutMs = 30 * 60 * 1000;
// Grbl fork 中 M3/M5 笔控会同步 planner 后才返回 ok。页尾 M5 可能排在整页
// 运动队列之后，等待时间等同剩余绘图时间，不能按普通 M-code 的 60s 判超时。
constexpr uint32_t kPlannerSyncOkTimeoutMs = kJobIdleTimeoutMs;
constexpr uint32_t kPostPaperIdleTimeoutMs = 5000;
constexpr uint32_t kReconnectReadyTimeoutMs = 2 * 60 * 1000;
constexpr uint32_t kResetRecoveryTimeoutMs = 30000;
constexpr uint32_t kPaperStatusTimeoutMs = 5000;
constexpr int kDeferredMaxRetries = 8;
constexpr int kDisconnectReplayMaxRetries = 2;
// 暂停上限：超过则自动放弃，避免 busy_ 被无限期占用导致所有工具返回 busy。
constexpr uint32_t kMaxPauseMs = 10 * 60 * 1000;

std::string JsonString(const char* value) {
    cJSON* root = cJSON_CreateString(value);
    if (root == nullptr) {
        return "\"\"";
    }
    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string json = str ? str : "\"\"";
    cJSON_free(str);
    return json;
}
}  // namespace

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
    if (line.rfind("M30", 0) == 0)
        return true;
    if (line.rfind("M721", 0) == 0)
        return true;
    if (line.rfind("M701", 0) == 0)
        return true;
    if (line.find("[ESP910]") != std::string::npos)
        return true;
    return false;
}

bool Job::LooksLikeMotionLine(const std::string& line) {
    if (line.empty())
        return false;
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
    if (c != 'G')
        return false;
    // G0–G3
    if (line.size() >= 2 && line[1] >= '0' && line[1] <= '3') {
        if (line.size() == 2 || !std::isdigit(static_cast<unsigned char>(line[2]))) {
            return true;
        }
    }
    return false;
}

static bool LooksLikePlannerSyncLine(const std::string& line) {
    if (line.size() < 2) {
        return false;
    }
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(line[0])));
    if (c != 'M') {
        return false;
    }
    // MCP 产物中的笔控行为：M3=落笔，M5=抬笔。Grbl 侧会等待前序 planner
    // 执行到该笔控点后才回 ok，因此行级等待窗口必须覆盖剩余绘图时间。
    if ((line[1] == '3' || line[1] == '5') &&
        (line.size() == 2 || !std::isdigit(static_cast<unsigned char>(line[2])))) {
        return true;
    }
    return false;
}

std::string Job::StartDraw(const std::string& url) {
    if (url.empty()) {
        return "{\"error\":\"url 不能为空\"}";
    }
    if (busy_.exchange(true)) {
        return JsonString("busy");
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
    return JsonString("started");
}

std::string Job::RequestAbort() {
    if (!busy_.load()) {
        return JsonString("ok");
    }
    bool paper_active = false;
    {
        // 与 ChangePaperAfterDraw() 的 M30 提交点互斥：要么 abort 先占位，
        // 要么 M30 已进入不可即停的换纸阶段，不能在两者之间误发软复位。
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        abort_requested_.store(true);
        paper_active = paper_active_.load();
    }
    std::string state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state = state_;
    }
    // 下载/校验尚未向 Grbl 下发绘图载荷，任务循环看到 abort 后自行收尾即可；
    // 此时软复位 Grbl 只会无谓丢坐标和触发重新探活。
    if (state == "downloading" || state == "verifying") {
        return JsonString("ok");
    }
    if (paper_active) {
        Notify("写字机正在换纸，无法即停；换纸完成后任务将停止");
        return JsonString("换纸中无法即停，完成后即停");
    }
    // 不在 MCP 回调里 sleep：独立短任务发 !，等 Hold 停稳后 0x18 丢弃 planner。
    // 对端复位 banner 会触发 Pipe 重新探活，探活序列的首个 M5 负责确定抬笔。
    xTaskCreate(
        [](void*) {
            auto& pipe = Pipe::GetInstance();
            auto& job = Job::GetInstance();
            pipe.SendRealtime('!');
            for (int i = 0; i < 80 && !job.IsPaperActive(); ++i) {
                pipe.SendRealtime('?');
                vTaskDelay(pdMS_TO_TICKS(100));
                GrblState state = pipe.GetGrblState();
                if (state == GrblState::Hold || state == GrblState::Idle) {
                    break;
                }
            }
            if (!job.IsPaperActive()) {
                pipe.PrepareAbortReset();
                if (!pipe.SendRealtime(static_cast<char>(0x18))) {
                    pipe.CancelAbortReset();
                }
            }
            vTaskDelete(nullptr);
        },
        "hutuji_abort", 2048, nullptr, 6, nullptr);
    return JsonString("ok");
}

std::string Job::RequestPause() {
    if (!busy_.load()) {
        return "{\"error\":\"当前没在出图\"}";
    }
    if (pen_test_active_.load()) {
        return JsonString("正在试笔，请稍候");
    }
    if (paper_active_.load()) {
        return JsonString("换纸中无法暂停，换纸完成后可再试");
    }
    // 与 SendLine 持同一把锁：`!` 发出后不能再有下一行进入 planner。
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
    if (paused_.exchange(true)) {
        return JsonString("已经是暂停状态");
    }
    // `!` 进给保持：Grbl 减速停住，planner 内容保留，`~` 可原地续跑。
    // 发送失败时回滚本地状态，不能把断链说成已暂停。
    if (!Pipe::GetInstance().SendRealtime('!')) {
        paused_.store(false);
        return "{\"error\":\"写字机 Telnet 已断开，无法暂停\"}";
    }
    // Hold 中普通 G-code 只会进入 planner，无法即时抬笔；额外注入 Z 行还会让主任务的
    // ok 配对发生漂移。暂停只使用 Grbl 实时字符，笔停在原地，`~` 后原位续跑。
    SetState("paused");
    if (auto* d = Board::GetInstance().GetDisplay())
        d->SetStatus("已暂停");
    return JsonString("ok");
}

std::string Job::RequestResume() {
    if (!busy_.load()) {
        return "{\"error\":\"当前没在出图\"}";
    }
    // 与 RequestPause 对称：试笔期间两个工具给同一个解释。
    if (pen_test_active_.load()) {
        return JsonString("正在试笔，请稍候");
    }
    if (!paused_.exchange(false)) {
        return JsonString("本来就没暂停");
    }
    if (!Pipe::GetInstance().SendRealtime('~')) {
        paused_.store(true);
        return "{\"error\":\"写字机 Telnet 已断开，无法继续\"}";
    }
    SetState("streaming");
    if (auto* d = Board::GetInstance().GetDisplay())
        d->SetStatus("继续画...");
    return JsonString("ok");
}

std::string Job::RequestRepeat() {
    if (busy_.exchange(true)) {
        return JsonString("busy");
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
    return replay ? JsonString("started") : JsonString("started_redownload");
}

std::string Job::RequestPenTest() {
    // 试笔也必须独占 Telnet 写入，避免与新出图的首行或重画交错。
    if (busy_.exchange(true)) {
        return JsonString("busy");
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
            if (p.SendLine("M5") && p.WaitOk(3000) && p.SendLine("M3 S1000") && p.WaitOk(3000)) {
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
    return JsonString("started");
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
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("重画中...");
            ESP_LOGI(TAG, "重画：复用 PSRAM %zu 字节，跳过下载/校验", buffer_len_);
        } else {
            SetState("downloading");
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("下载中...");
            if (!DownloadToPsram(url_)) {
                if (abort_requested_.load()) {
                    SetState("aborted");
                    if (auto* d = Board::GetInstance().GetDisplay())
                        d->SetStatus("已取消");
                } else {
                    SetState("error");
                    Notify(std::string("下载失败: ") + last_error_);
                }
                break;
            }
            if (abort_requested_.load()) {
                SetState("aborted");
                break;
            }
            SetState("verifying");
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("校验中...");
            if (!VerifyCrc()) {
                SetState("error");
                Notify(std::string("校验失败: ") + last_error_);
                break;
            }
        }
        auto& pipe = Pipe::GetInstance();
        // 从这里到任务收尾保护整个绘图会话：若中途 TCP 重连，Pipe 只能验 banner，
        // 禁止在旧 planner/换纸状态未知时自动发送 M5/G1 授权探针。
        pipe.SetTaskSessionActive(true);
        if (!pipe.IsReady()) {
            last_error_ = "写字机未就绪";
            SetState("error");
            Notify(last_error_);
            break;
        }
        if (!pipe.IsAuthorized()) {
            last_error_ = "写字机未授权，请先按设备授权 SOP 处理";
            SetState("error");
            Notify(last_error_);
            break;
        }
        // 授权探测已在 Pipe 建链时完成；未授权任务在这里停止，绘图载荷零字节下发。
        // 下载/校验期间就被暂停时，不能把状态改回 streaming——否则 status 谎报
        // 正在画，实际转发循环一进去就卡在暂停门上。
        int disconnect_replays = 0;
        while (true) {
            SetStreamingOrPaused();
            ok = StreamToGrbl();
            if (ok) {
                ok = WaitForIdle(true, kJobIdleTimeoutMs);
            }
            if (ok || abort_requested_.load() || !stream_disconnected_) {
                break;
            }
            if (++disconnect_replays > kDisconnectReplayMaxRetries) {
                last_error_ = "断连自动重画次数耗尽";
                break;
            }
            if (!RecoverDisconnectedDraw()) {
                break;
            }
            ESP_LOGW(TAG, "断连恢复完成，从 PSRAM 第 1 行重画（%d/%d）", disconnect_replays,
                     kDisconnectReplayMaxRetries);
        }
        if (ok) {
            ok = ChangePaperAfterDraw();
        }
        if (abort_requested_.load()) {
            SetState("aborted");
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("已取消");
        } else if (ok) {
            SetState("done");
            Notify("出图完成");
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("画好啦！");
        } else {
            SetState("error");
            Notify(std::string("转发失败: ") + last_error_);
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("出错了");
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
    Pipe::GetInstance().SetTaskSessionActive(false);
    paused_.store(false);
    repeat_mode_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ESP_LOGI(TAG, "任务结束 state=%s err=%s replayable=%d", state_.c_str(), last_error_.c_str(),
                 (int)buffer_replayable_.load());
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
        last_error_ =
            "长度不符 expect=" + std::to_string(content_length) + " got=" + std::to_string(total);
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

void Job::UpdateDisplayProgress() {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display)
        return;

    char buf[48];
    if (paper_active_.load()) {
        snprintf(buf, sizeof(buf), "换纸中... %zu/%zu", lines_sent_, lines_total_);
    } else {
        int pct = lines_total_ > 0 ? static_cast<int>(lines_sent_ * 100 / lines_total_) : 0;
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

bool Job::WaitForIdle(bool honor_abort, uint32_t timeout_ms) {
    auto& pipe = Pipe::GetInstance();
    const TickType_t began = xTaskGetTickCount();

    while (!honor_abort || !abort_requested_.load()) {
        if (honor_abort && !WaitWhilePaused()) {
            if (last_error_.empty())
                last_error_ = "aborted";
            return false;
        }
        if (!pipe.IsConnected() || !pipe.IsReady() ||
            (honor_abort && pipe.GetConnectionSequence() != stream_connection_seq_)) {
            if (honor_abort)
                stream_disconnected_ = true;
            last_error_ = "等待运动完成时链路丢失";
            return false;
        }
        if (pipe.GetGrblState() == GrblState::Alarm) {
            last_error_ = "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
            return false;
        }

        // Grbl 的 ok 只代表行已进入 planner。必须等到 `?` 之后解析到一份新报告；
        // 固定 sleep 后直接读状态会在 WiFi/任务调度超过该延迟时沿用旧 Idle。
        const uint32_t status_seq = pipe.GetStatusReportSequence();
        if (!pipe.SendRealtime('?')) {
            last_error_ = "查询运动完成状态失败";
            return false;
        }
        for (int i = 0; i < 20 && (!honor_abort || !abort_requested_.load()) &&
                        pipe.GetStatusReportSequence() == status_seq;
             ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (pipe.GetStatusReportSequence() != status_seq &&
            pipe.GetGrblState() == GrblState::Idle) {
            return true;
        }

        if ((xTaskGetTickCount() - began) >= pdMS_TO_TICKS(timeout_ms)) {
            last_error_ = "等待写字机运动完成超时";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (honor_abort) {
        last_error_ = "aborted";
    }
    return false;
}

bool Job::RecoverDisconnectedDraw() {
    auto& pipe = Pipe::GetInstance();
    SetState("reconnecting");
    // 新 session 的 Changing 尚未查明前按最保守的换纸保护处理。此窗口若收到 abort，
    // 只能置挂起，禁止另一任务误发 0x18；确认 Off 后再解除保护并执行受限 reset。
    paper_active_.store(true);
    if (auto* d = Board::GetInstance().GetDisplay())
        d->SetStatus("写字机重连中...");
    Notify("写字机连接中断，正在安全恢复这幅画");

    const TickType_t reconnect_began = xTaskGetTickCount();
    while (!abort_requested_.load()) {
        if (pipe.IsConnected() && pipe.GetConnectionSequence() != stream_connection_seq_) {
            break;
        }
        if ((xTaskGetTickCount() - reconnect_began) >= pdMS_TO_TICKS(kReconnectReadyTimeoutMs)) {
            last_error_ = "等待写字机重连超时";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (abort_requested_.load()) {
        last_error_ = "aborted";
        return false;
    }
    const uint32_t paper_seq = pipe.GetPaperStatusSequence();
    if (!pipe.SendLine("[ESP901]")) {
        last_error_ = "重连后查询换纸状态失败";
        return false;
    }
    int paper_err = -1;
    WaitResult paper_wr = pipe.WaitResponse(kPaperStatusTimeoutMs, nullptr, &paper_err);
    if (paper_wr != WaitResult::Ok || pipe.GetPaperStatusSequence() == paper_seq) {
        last_error_ = paper_wr == WaitResult::Failed
                          ? "重连后查询换纸状态失败 (error:" + std::to_string(paper_err) + ")"
                          : "重连后未收到有效 Changing 状态";
        return false;
    }
    if (pipe.GetPaperChangingState() != PaperChangingState::Off) {
        last_error_ = "断连发生在换纸窗口，已停止自动恢复，请检查纸张后重画";
        Notify(last_error_);
        // 不发 reset/M30。保留会话保护并轮询到换纸退出，使 Pipe 不会在
        // Changing=On 时自动跑 G1 授权探针；任务仍以 error 结束，不会续画。
        const TickType_t changing_began = xTaskGetTickCount();
        while (pipe.IsConnected() &&
               (xTaskGetTickCount() - changing_began) < pdMS_TO_TICKS(kPaperOkTimeoutMs)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            const uint32_t seq = pipe.GetPaperStatusSequence();
            if (!pipe.SendLine("[ESP901]")) {
                break;
            }
            int query_err = -1;
            if (pipe.WaitResponse(kPaperStatusTimeoutMs, nullptr, &query_err) == WaitResult::Ok &&
                pipe.GetPaperStatusSequence() != seq &&
                pipe.GetPaperChangingState() == PaperChangingState::Off) {
                break;
            }
        }
        return false;
    }
    paper_active_.store(false);

    // 普通画线断连：只给本次主动 reset 自动解锁权限。reset 清掉可能仍在执行的 planner，
    // 探活序列再明确抬笔并恢复授权；任何其它 reset/Alarm 仍保持人工处理。
    const uint32_t reset_seq = pipe.GetResetBannerSequence();
    pipe.PrepareAbortReset();
    if (!pipe.SendRealtime(static_cast<char>(0x18))) {
        pipe.CancelAbortReset();
        last_error_ = "断连恢复软复位发送失败";
        return false;
    }

    const TickType_t reset_began = xTaskGetTickCount();
    while (!abort_requested_.load()) {
        if (pipe.GetResetBannerSequence() != reset_seq && pipe.IsReady()) {
            break;
        }
        if ((xTaskGetTickCount() - reset_began) >= pdMS_TO_TICKS(kResetRecoveryTimeoutMs)) {
            last_error_ = "断连恢复后重新探活超时";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (abort_requested_.load()) {
        last_error_ = "aborted";
        return false;
    }
    if (!pipe.IsAuthorized()) {
        last_error_ = "断连恢复后写字机未授权";
        return false;
    }

    // 走掉已经画坏的纸并装入新纸。成功后 ChangePaperAfterDraw 已确认 fresh Idle。
    if (!ChangePaperAfterDraw()) {
        if (!abort_requested_.load()) {
            last_error_ = "断连废纸处理失败: " + last_error_;
        }
        return false;
    }
    stream_disconnected_ = false;
    Notify("写字机已恢复，正在从头重画");
    return true;
}

bool Job::ChangePaperAfterDraw() {
    auto& pipe = Pipe::GetInstance();

    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        paper_active_.store(true);
        SetState("paper_change");
        if (auto* d = Board::GetInstance().GetDisplay())
            d->SetStatus("换纸中...");

        // M30 是 S3 的页结束编排命令，不属于云端下载并校验的 G-code 文件。
        // SendLine 会清理旧响应，随后只等待这一条 M30 的最终 ok/error。
        if (!pipe.SendLine("M30")) {
            paper_active_.store(false);
            last_error_ = "自动换纸命令发送失败";
            return false;
        }
    }

    WaitResult wr = WaitResult::Timeout;
    int err = -1;
    uint32_t waited = 0;
    constexpr uint32_t kSliceMs = 1000;
    while (waited < kPaperOkTimeoutMs) {
        if (!pipe.IsConnected() || !pipe.IsReady()) {
            paper_active_.store(false);
            last_error_ = "自动换纸时链路丢失";
            return false;
        }
        if (pipe.GetGrblState() == GrblState::Alarm) {
            paper_active_.store(false);
            last_error_ =
                "自动换纸时写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
            return false;
        }
        const uint32_t step =
            (kPaperOkTimeoutMs - waited > kSliceMs) ? kSliceMs : (kPaperOkTimeoutMs - waited);
        wr = pipe.WaitResponse(step, nullptr, &err);
        if (wr != WaitResult::Timeout) {
            break;
        }
        waited += step;
    }

    if (wr != WaitResult::Ok) {
        if (wr == WaitResult::Failed || wr == WaitResult::Deferred) {
            last_error_ = "自动换纸失败 (error:" + std::to_string(err) + ")";
        } else {
            pipe.SendRealtime('?');
            last_error_ = "等待自动换纸完成超时";
        }
        paper_active_.store(false);
        return false;
    }

    // M30 的 ok 只说明阻塞式换纸函数已经返回；再取一份新状态报告，
    // 确认写字机确实回到 Idle 后才允许任务进入 done。
    if (!WaitForIdle(false, kPostPaperIdleTimeoutMs)) {
        last_error_ = "自动换纸后未确认 Idle: " + last_error_;
        paper_active_.store(false);
        return false;
    }

    paper_active_.store(false);
    if (abort_requested_.load()) {
        last_error_ = "aborted";
        return false;
    }
    return true;
}

std::vector<Job::LineSpan> Job::ParseLines() const {
    std::vector<LineSpan> spans;
    size_t i = 0;
    while (i < buffer_len_) {
        size_t start = i;
        while (i < buffer_len_ && buffer_[i] != '\n' && buffer_[i] != '\r') {
            ++i;
        }
        size_t end = i;  // [start, end) = 本行原始内容（不含换行）
        while (i < buffer_len_ && (buffer_[i] == '\n' || buffer_[i] == '\r')) {
            ++i;
        }

        // 以下三步与改造前 StreamToGrbl 的内联逻辑逐字等价，只是改为移动边界
        // 而非拷贝字符串：① 剥 `;` 注释 ② 剥尾部空白 ③ 剥首部空白。
        for (size_t k = start; k < end; ++k) {
            if (buffer_[k] == ';') {
                end = k;
                break;
            }
        }
        while (end > start && (buffer_[end - 1] == ' ' || buffer_[end - 1] == '\t')) {
            --end;
        }
        while (start < end && (buffer_[start] == ' ' || buffer_[start] == '\t')) {
            ++start;
        }
        if (start == end) {
            continue;  // 空行/纯注释行不转发（改造前同样跳过，不计入 lines_total_）
        }
        spans.push_back(LineSpan{static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)});
    }
    return spans;
}

bool Job::StreamToGrbl() {
    auto& pipe = Pipe::GetInstance();
    stream_disconnected_ = false;
    stream_connection_seq_ = pipe.GetConnectionSequence();

    // S2：先预解析成行索引，获得 peek 能力（窗口化流控前提）。
    // 窗口仍 = 1（逐行等 ok），行为与改造前一致；打开窗口是 S3 的事。
    const std::vector<LineSpan> spans = ParseLines();
    lines_total_ = spans.size();
    lines_sent_ = 0;
    UpdateDisplayProgress();
    TickType_t last_notify_tick = xTaskGetTickCount();

    for (size_t idx = 0; idx < spans.size(); ++idx) {
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        // 暂停门：停在行边界，不切断已发出的行。Grbl 侧由 `!` 做进给保持，
        // 这里只是不再灌新行，避免暂停期间 planner 继续被填满。
        if (!WaitWhilePaused()) {
            if (last_error_.empty())
                last_error_ = "aborted";
            return false;
        }
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        if (!pipe.IsConnected() || !pipe.IsReady() ||
            pipe.GetConnectionSequence() != stream_connection_seq_) {
            stream_disconnected_ = true;
            last_error_ = "转发中链路丢失";
            return false;
        }
        if (pipe.GetGrblState() == GrblState::Alarm) {
            last_error_ = "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
            return false;
        }

        // 只为当前要发的这一行做一次拷贝；索引表本身零拷贝指向 buffer_。
        const std::string line(LineAt(spans[idx]));

        bool paper_line = LooksLikePaperLine(line);
        if (paper_line) {
            paper_active_.store(true);
            SetState("paper_change");
        }

        int retries = 0;
        while (true) {
            // 行已经解析但尚未写入时也可能收到 pause；在这里等待而不是忙等重试。
            if (!WaitWhilePaused()) {
                if (last_error_.empty())
                    last_error_ = "aborted";
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
                    stream_disconnected_ = !pipe.IsConnected() ||
                                           pipe.GetConnectionSequence() != stream_connection_seq_;
                    last_error_ = stream_disconnected_ ? "转发中链路丢失" : "SendLine 失败";
                    return false;
                }
            }

            bool planner_sync_line = LooksLikePlannerSyncLine(line);
            uint32_t timeout =
                paper_line ? kPaperOkTimeoutMs
                           : (planner_sync_line ? kPlannerSyncOkTimeoutMs
                                                : (LooksLikeMotionLine(line) ? kMotionOkTimeoutMs
                                                                             : kOkTimeoutMs));
            // 分段等，便于响应 abort
            WaitResult wr = WaitResult::Timeout;
            int err = -1;
            uint32_t waited = 0;
            const uint32_t slice = 1000;
            while (waited < timeout) {
                // 当前行已发送时也要冻结等待超时；否则暂停超过 timeout 会被误判为转发失败。
                if (!WaitWhilePaused()) {
                    if (last_error_.empty())
                        last_error_ = "aborted";
                    return false;
                }
                if (abort_requested_.load() && !paper_active_.load()) {
                    last_error_ = "aborted";
                    return false;
                }
                if (!pipe.IsConnected() || !pipe.IsReady() ||
                    pipe.GetConnectionSequence() != stream_connection_seq_) {
                    stream_disconnected_ = true;
                    last_error_ = "等待应答时链路丢失";
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
                    int pct =
                        lines_total_ > 0 ? static_cast<int>(lines_sent_ * 100 / lines_total_) : 0;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "出图进度: %d%% (%zu/%zu行) 位置 X=%.1f Y=%.1f", pct,
                             lines_sent_, lines_total_, mx, my);
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

}  // namespace hutuji
