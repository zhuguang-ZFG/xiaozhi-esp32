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
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <deque>
#include <string>

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
constexpr uint32_t kPenOriginIdleTimeoutMs = 5000;
// 等 ok 超时后的状态兜底探测：只要一份新状态报告，2s 足够（`?` 是实时命令，
// 不排 planner 队列）。
constexpr uint32_t kOkFallbackIdleTimeoutMs = 2000;
// 兜底次数上限。Telnet 偶发吃 ok 每页最多出现个别次；真丢行/真卡死必须暴露成
// 失败，不能被兜底无限掩盖。
constexpr int kMaxOkFallback = 3;
// MPos 与在途行终点的比较容差（mm）。Grbl 报告保留 3 位小数。
constexpr float kOkFallbackPosTolMm = 0.05f;
// 本机 $1=25ms：Idle 后等待驱动失能和弹簧回到自然抬笔位，再声明 Z0。
constexpr uint32_t kPenSpringReturnMs = 100;
constexpr uint32_t kReconnectReadyTimeoutMs = 2 * 60 * 1000;
constexpr uint32_t kResetRecoveryTimeoutMs = 30000;
constexpr uint32_t kPaperStatusTimeoutMs = 5000;
constexpr int kDeferredMaxRetries = 8;
constexpr int kDisconnectReplayMaxRetries = 2;
// 暂停上限：超过则自动放弃，避免 busy_ 被无限期占用导致所有工具返回 busy。
constexpr uint32_t kMaxPauseMs = 10 * 60 * 1000;
// S3 窗口化流控窗口（§3 取值：Telnet RX ①的 43%，约 21 行在途）。
constexpr size_t kWindow = 512;

// 设备侧出图只允许云端生成的 capability URL；scheme 限定 http/https，
// 主机限定私有网段或 hutuji.donglicao.com。拦坏 url 在设备侧，
// 避免 MCP 回调被诱发出站拉取任意内网/公网地址。
static bool IsValidDrawUrl(const std::string& url) {
    const std::string kHttps = "https://";
    const std::string kHttp = "http://";
    std::string rest;
    if (url.rfind(kHttps, 0) == 0) {
        rest = url.substr(kHttps.size());
    } else if (url.rfind(kHttp, 0) == 0) {
        rest = url.substr(kHttp.size());
    } else {
        return false;
    }
    const size_t slash = rest.find('/');
    const std::string hostport = rest.substr(0, slash);
    if (hostport.empty()) {
        return false;
    }
    std::string host = hostport;
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        return false;
    }
    std::string lowhost = host;
    std::transform(lowhost.begin(), lowhost.end(), lowhost.begin(), ::tolower);
    if (lowhost == "hutuji.donglicao.com") {
        return true;
    }
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    if (a == 10)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    if (a == 192 && b == 168)
        return true;
    return false;
}

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

/**
 * 从一行 G-code 里取某个字母词的数值（注释与首尾空白已由 ParseLines 剥掉）。
 * @return true 找到并解析成功
 */
static bool ExtractGcodeWord(std::string_view line, char letter, float& out) {
    const char* end = line.data() + line.size();
    for (size_t i = 0; i < line.size(); ++i) {
        if (static_cast<char>(std::toupper(static_cast<unsigned char>(line[i]))) != letter) {
            continue;
        }
        const char* p = line.data() + i + 1;
        while (p < end && *p == ' ') {
            ++p;
        }
        char buf[32];
        size_t n = 0;
        while (p < end && n + 1 < sizeof(buf) &&
               (std::isdigit(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-' ||
                *p == '.')) {
            buf[n++] = *p++;
        }
        if (n == 0) {
            continue;
        }
        buf[n] = '\0';
        char* endp = nullptr;
        float v = std::strtof(buf, &endp);
        if (endp == buf) {
            continue;
        }
        out = v;
        return true;
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
    if (!IsValidDrawUrl(url)) {
        return "{\"error\":\"url 必须是 http(s)://<私有IP或hutuji.donglicao.com>/... 的 capability "
               "地址\"}";
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
    bool pen_test_active = false;
    bool paper_active = false;
    {
        // busy/试笔/换纸与 abort 在同一提交锁下取快照，避免试笔启停窗口误走 0x18。
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (!busy_.load()) {
            return JsonString("ok");
        }
        abort_requested_.store(true);
        pen_test_active = pen_test_active_.load();
        paper_active = paper_active_.load();
    }
    // 试笔只有一组已界定的短 Z 运动。若已整批提交，让它完成 Z0 抬笔比并发
    // 0x18、触发授权探测抢应答更安全。
    if (pen_test_active) {
        return JsonString("试笔即将停止");
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
    // busy/试笔/换纸与暂停提交在同一快照下判断；`!` 发出后不能再有普通行入 planner。
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
    if (!busy_.load()) {
        return "{\"error\":\"当前没在出图\"}";
    }
    if (pen_test_active_.load()) {
        return JsonString("正在试笔，请稍候");
    }
    if (paper_active_.load()) {
        return JsonString("换纸中无法暂停，换纸完成后可再试");
    }
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
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
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
    auto& pipe = Pipe::GetInstance();
    {
        // 试笔状态与 busy 一起发布，RequestAbort 在同一把锁下分流，不能看到半成品状态。
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (busy_.load()) {
            return JsonString("busy");
        }
        if (!pipe.IsConnected() || !pipe.IsReady() || !pipe.IsAuthorized()) {
            return "{\"error\":\"写字机未连接、未就绪或未授权\"}";
        }
        abort_requested_.store(false);
        busy_.store(true);
        pen_test_active_.store(true);
        stream_connection_seq_ = pipe.GetConnectionSequence();
        SetState("pen_test");
    }

    // 不在 MCP 回调里阻塞：独立短任务按奎享 Stepper 链路做 Z0 校准 → Z5 → Z0。
    BaseType_t created = xTaskCreate(
        [](void*) {
            auto& p = Pipe::GetInstance();
            auto& job = Job::GetInstance();
            bool ok = job.PreparePenOrigin();
            bool motion_attempted = false;
            int submitted = 0;
            if (ok) {
                // Z5 用 F1000（约 300ms）而不是 F10000 的机械瞬时：两条普通行仍一次
                // 一个应答，但抬笔行能在 25ms 失能窗口前进入 planner。
                p.DrainResponses();
                p.SetWindowed(true);
                struct WindowGuard {
                    Pipe& pipe;
                    ~WindowGuard() { pipe.SetWindowed(false); }
                } guard{p};
                {
                    std::lock_guard<std::mutex> stream_lock(job.stream_mutex_);
                    if (job.abort_requested_.load()) {
                        ok = false;
                    } else if (!p.IsConnected() || !p.IsReady() || !p.IsAuthorized() ||
                               p.GetConnectionSequence() != job.stream_connection_seq_) {
                        ok = false;
                    } else {
                        motion_attempted = true;
                        if (p.SendLine("G1G90 Z5.0F1000")) {
                            ++submitted;
                            if (p.SendLine("G1G90 Z0.0F10000")) {
                                ++submitted;
                            } else {
                                ok = false;
                            }
                        } else {
                            ok = false;
                        }
                    }
                }
                for (int i = 0; i < submitted; ++i) {
                    if (p.TakeResponse(3000) != WaitResult::Ok) {
                        ok = false;
                    }
                }
            }
            // 已尝试运动时即使用户随后 abort，也等 Z0 收尾完成/链路失效；RequestAbort
            // 不会对试笔并发软复位，这里只等待，不再发送普通命令。
            if (motion_attempted) {
                ok = job.WaitForIdle(false, kPenOriginIdleTimeoutMs) && ok;
            }

            bool aborted;
            {
                // 状态与 busy 一起撤销，杜绝 abort 在收尾缝隙误走普通绘图的 0x18 路径。
                std::lock_guard<std::mutex> stream_lock(job.stream_mutex_);
                aborted = job.abort_requested_.load();
                job.SetState(aborted ? "aborted" : ok ? "done" : "error");
                job.pen_test_active_.store(false);
                job.busy_.store(false);
            }
            job.Notify(aborted ? "笔测试已停止"
                       : ok    ? "笔测试完成：请确认笔头完成一次触纸再抬起"
                               : "笔测试失败：写字机没正常应答");
            vTaskDelete(nullptr);
        },
        "hutuji_pentest", 3072, nullptr, 5, nullptr);
    if (created != pdTRUE) {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        pen_test_active_.store(false);
        busy_.store(false);
        SetState("idle");
        return "{\"error\":\"无法创建试笔任务\"}";
    }
    return JsonString("started");
}

std::string Job::StatusJson() const {
    auto& pipe = Pipe::GetInstance();
    // 发 ? 触发后台更新，不在 MCP 回调里 sleep 等待；直接返回当前缓存状态。
    if (pipe.IsConnected()) {
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
    // 净作画时长与 ETA（对齐奎享 f.java:j()/h()）。仅 streaming/paused/paper_change
    // 有意义；下载/校验/idle 不报，避免误导。
    if (draw_start_tick_ != 0 && lines_total_ > 0) {
        uint32_t elapsed_ms =
            static_cast<uint32_t>((xTaskGetTickCount() - draw_start_tick_) * portTICK_PERIOD_MS);
        uint32_t net_ms = elapsed_ms > paused_accum_ms_ ? elapsed_ms - paused_accum_ms_ : 0;
        cJSON_AddNumberToObject(root, "elapsed_ms", net_ms);
        if (lines_sent_ > 0) {
            uint32_t eta_ms =
                static_cast<uint32_t>(static_cast<uint64_t>(net_ms) * lines_total_ / lines_sent_);
            cJSON_AddNumberToObject(root, "eta_ms", eta_ms);
        }
    }
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
        const uint32_t session_seq = pipe.GetConnectionSequence();
        if (!pipe.IsConnected() || !pipe.IsReady() || pipe.GetConnectionSequence() != session_seq) {
            last_error_ = "写字机未就绪或连接正在切换";
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
        // 双读序号包住 ready/authorized 检查，防止采到刚重连但尚未探活的新 session。
        if (pipe.GetConnectionSequence() != session_seq) {
            last_error_ = "写字机连接在任务启动时发生切换";
            SetState("error");
            Notify(last_error_);
            break;
        }
        stream_connection_seq_ = session_seq;
        // 授权探测已在 Pipe 建链时完成；未授权任务在这里停止，绘图载荷零字节下发。
        // 下载/校验期间就被暂停时，不能把状态改回 streaming——否则 status 谎报
        // 正在画，实际转发循环一进去就卡在暂停门上。
        int disconnect_replays = 0;
        while (true) {
            SetStreamingOrPaused();
            // 奎享完整会话会在首个笔控前旁路 G92 Z0；下载文件本身仍不含 G92。
            // 每轮重画也必须重做：电机失能后弹簧已回位，Grbl 旧 Z 计数不再可信。
            ok = PreparePenOrigin();
            if (ok) {
                ok = StreamToGrbl();
            }
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
    Pipe::GetInstance().SetExpectBlockingPeer(false);
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
    pause_segment_start_ = began;
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
            // Grbl 仍在 Hold（RequestPause 发的 !），planner 满且笔可能压在纸上。
            // 必须冲掉 planner 否则后续 PreparePenOrigin 永远等不到 Idle。
            pipe.SendRealtime('!');
            for (int i = 0; i < 40; ++i) {
                pipe.SendRealtime('?');
                vTaskDelay(pdMS_TO_TICKS(100));
                GrblState st = pipe.GetGrblState();
                if (st == GrblState::Hold || st == GrblState::Idle) break;
            }
            pipe.PrepareAbortReset();
            pipe.SendRealtime(static_cast<char>(0x18));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    // 累加本次暂停段，供 ETA 扣减（对齐奎享 g 字段）。
    paused_accum_ms_ +=
        static_cast<uint32_t>((xTaskGetTickCount() - pause_segment_start_) * portTICK_PERIOD_MS);
    return !abort_requested_.load();
}

bool Job::PreparePenOrigin() {
    auto& pipe = Pipe::GetInstance();
    stream_disconnected_ = false;

    while (!abort_requested_.load()) {
        if (!WaitForIdle(true, kPenOriginIdleTimeoutMs)) {
            if (last_error_.empty()) {
                last_error_ = "校准 Z0 前未确认写字机 Idle";
            } else if (last_error_ != "aborted") {
                last_error_ = "校准 Z0 前未确认写字机 Idle: " + last_error_;
            }
            return false;
        }

        // `$1=25` 会在 Idle 25ms 后关闭驱动；再留足时间让弹簧回到自然抬笔位。
        vTaskDelay(pdMS_TO_TICKS(kPenSpringReturnMs));
        bool retry_after_pause = false;
        {
            // 与 RequestPause/RequestAbort 共用提交锁：若 `!`/abort 先发生，本轮不得再发 G92。
            std::lock_guard<std::mutex> stream_lock(stream_mutex_);
            if (abort_requested_.load()) {
                last_error_ = "aborted";
                return false;
            }
            if (paused_.load()) {
                retry_after_pause = true;
            } else if (!pipe.IsConnected() || !pipe.IsReady() ||
                       pipe.GetConnectionSequence() != stream_connection_seq_) {
                stream_disconnected_ = true;
                last_error_ = "校准 Z0 前链路丢失";
                return false;
            } else if (!pipe.SendLine("G92 Z0")) {
                stream_disconnected_ = true;
                last_error_ = "发送 Z0 校准命令失败";
                return false;
            }
        }
        if (retry_after_pause) {
            if (!WaitWhilePaused()) {
                stream_disconnected_ = !pipe.IsConnected() || !pipe.IsReady() ||
                                       pipe.GetConnectionSequence() != stream_connection_seq_;
                if (abort_requested_.load()) {
                    last_error_ = "aborted";
                }
                return false;
            }
            continue;
        }

        int error_code = -1;
        const WaitResult wr = pipe.WaitResponse(kPaperStatusTimeoutMs, nullptr, &error_code);
        if (wr != WaitResult::Ok) {
            stream_disconnected_ = !pipe.IsConnected() || !pipe.IsReady() ||
                                   pipe.GetConnectionSequence() != stream_connection_seq_;
            last_error_ = wr == WaitResult::Timeout
                              ? "等待 Z0 校准应答超时"
                              : "Z0 校准被 Grbl 拒绝 (error:" + std::to_string(error_code) + ")";
            return false;
        }
        ESP_LOGI(TAG, "弹簧回位后已校准 G92 Z0");
        return true;
    }

    last_error_ = "aborted";
    return false;
}

bool Job::ConfirmInFlightDoneByStatus(const std::vector<LineSpan>& spans, size_t from, size_t to) {
    auto& pipe = Pipe::GetInstance();
    if (from >= to || to > spans.size()) {
        return false;
    }
    if (!pipe.IsConnected() || !pipe.IsReady() ||
        pipe.GetConnectionSequence() != stream_connection_seq_) {
        return false;
    }

    // 必须是 `?` 之后新解析出来的报告，否则会沿用超时前的旧 Idle。
    const uint32_t status_seq = pipe.GetStatusReportSequence();
    if (!pipe.SendRealtime('?')) {
        return false;
    }
    const TickType_t began = xTaskGetTickCount();
    while (pipe.GetStatusReportSequence() == status_seq) {
        if ((xTaskGetTickCount() - began) >= pdMS_TO_TICKS(kOkFallbackIdleTimeoutMs)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (pipe.GetGrblState() != GrblState::Idle) {
        return false;  // 还在 Run/Hold：ok 没丢，是真没执行完
    }

    // 在途行里最后出现的 X / Y 目标（下载文件恒为 G90 绝对坐标，protocol §5）。
    bool have_x = false, have_y = false;
    float tx = 0.0f, ty = 0.0f;
    for (size_t i = to; i > from; --i) {
        const std::string_view sv = LineAt(spans[i - 1]);
        float v = 0.0f;
        if (!have_x && ExtractGcodeWord(sv, 'X', v)) {
            tx = v;
            have_x = true;
        }
        if (!have_y && ExtractGcodeWord(sv, 'Y', v)) {
            ty = v;
            have_y = true;
        }
        if (have_x && have_y) {
            break;
        }
    }
    if (!have_x && !have_y) {
        return true;  // 纯 Z / M / 模态行：Idle 本身就证明 planner 已排空
    }

    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    pipe.GetMachinePos(mx, my, mz);
    if (have_x && std::fabs(mx - tx) > kOkFallbackPosTolMm) {
        return false;
    }
    if (have_y && std::fabs(my - ty) > kOkFallbackPosTolMm) {
        return false;
    }
    return true;
}

bool Job::WaitForIdle(bool honor_abort, uint32_t timeout_ms) {
    auto& pipe = Pipe::GetInstance();
    const TickType_t began = xTaskGetTickCount();

    while (!honor_abort || !abort_requested_.load()) {
        if (honor_abort && !WaitWhilePaused()) {
            stream_disconnected_ = !pipe.IsConnected() || !pipe.IsReady() ||
                                   pipe.GetConnectionSequence() != stream_connection_seq_;
            if (last_error_.empty()) {
                last_error_ = abort_requested_.load() ? "aborted" : "等待暂停恢复失败";
            }
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
            if (honor_abort) {
                stream_disconnected_ = true;
            }
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
        // 换纸窗口内同样打开 blocking 标记，避免等待期间 silent-poll 误杀 TCP。
        pipe.SetExpectBlockingPeer(true);
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
    const uint32_t recovered_seq = pipe.GetConnectionSequence();
    if (!pipe.IsConnected() || !pipe.IsReady() || !pipe.IsAuthorized() ||
        pipe.GetConnectionSequence() != recovered_seq) {
        last_error_ = "断连恢复完成时连接再次切换";
        return false;
    }
    stream_connection_seq_ = recovered_seq;
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
        // 换纸期间 Grbl 主循环不转，PipeTask 的 `?` 收不到状态行；
        // 提前打开 blocking 标记，避免 silent-poll≈21s 误杀 TCP。
        pipe.SetExpectBlockingPeer(true);
        if (!pipe.SendLine("M30")) {
            pipe.SetExpectBlockingPeer(false);
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
        pipe.SetExpectBlockingPeer(false);
        paper_active_.store(false);
        return false;
    }

    // M30 的 ok 只说明阻塞式换纸函数已经返回；再取一份新状态报告，
    // 确认写字机确实回到 Idle 后才允许任务进入 done。
    if (!WaitForIdle(false, kPostPaperIdleTimeoutMs)) {
        last_error_ = "自动换纸后未确认 Idle: " + last_error_;
        pipe.SetExpectBlockingPeer(false);
        paper_active_.store(false);
        return false;
    }

    pipe.SetExpectBlockingPeer(false);
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
    // stream_connection_seq_ 由 PreparePenOrigin() 在 G92 前锁定；这里不能覆盖，
    // 否则校准后发生的重连会被误认成同一会话。

    // S2：预解析成行索引，获得 peek 能力（窗口化流控前提）。
    const std::vector<LineSpan> spans = ParseLines();
    lines_total_ = spans.size();
    lines_sent_ = 0;
    paused_accum_ms_ = 0;
    draw_start_tick_ = xTaskGetTickCount();
    UpdateDisplayProgress();
    TickType_t last_notify_tick = xTaskGetTickCount();

    // S3：打开窗口化模式 — SendLine 不再清残留应答，在途应答是合法凭据。
    // 函数退出前（无论成功/失败/abort）必须复原为逐行模式，避免影响后续
    // status/pen_test 等单命令调用点。
    pipe.SetWindowed(true);
    struct WindowGuard {
        Pipe& pipe_ref;
        ~WindowGuard() { pipe_ref.SetWindowed(false); }
    } window_guard{pipe};

    // 照抄官方 stream.py 的 c_line / g_count 结构（§4.2）
    std::deque<size_t> c_line;    // 在途各行字节数（含 +1 换行，R1）
    size_t c_line_bytes_sum = 0;  // 在途字节数累加
    size_t next_ = 0;             // 下一条待发

    // paper_pending：遇到换纸行时先排空 c_line，排空后在此标记下走逐行模式。
    // 理由：换纸期主循环阻塞，若在途字节涌入会撑满 InputBuffer ②导致静默丢
    // 字节（§3）；换纸行 ok 等数十秒且期间禁发 G0–G3（§4.3 第一条）。
    bool paper_pending = false;
    // 等 ok 超时后靠状态报告兜底的次数。Grbl WebUI Telnet 输出无 TX 缓冲，`ok` 与
    // `?` 报告同核并发写同一 socket，偶发被抢占会静默吞掉一个 `ok`。超上限仍失败，
    // 避免真丢行/真卡死被无限掩盖。
    int ok_fallback_count = 0;

    // TODO(S4/Bf)：实机出图期周期性 ?，若 Telnet Bf 余量 < 300 则临时收窄窗口。
    //   自适应反压（§3.1）本次不做，S3 核心是 window=512 + 换纸行排空。

    while (next_ < spans.size() || !c_line.empty()) {
        // === 检查点（§4.3：abort/ALARM/链路从「每行」变成「每次灌/收循环」）===
        if (abort_requested_.load() && !paper_active_.load()) {
            // abort 后在途计数残留：c_line 随函数返回整体销毁，不需显式清零。
            last_error_ = "aborted";
            return false;
        }
        if (pipe.GetGrblState() == GrblState::Alarm) {
            last_error_ = "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
            return false;
        }
        if (!pipe.IsConnected() || !pipe.IsReady() ||
            pipe.GetConnectionSequence() != stream_connection_seq_) {
            stream_disconnected_ = true;
            last_error_ = "转发中链路丢失";
            return false;
        }

        // === 暂停（§4.3：停止灌新行，但必须继续收在途 ok）===
        // 暂停且无在途 → 阻塞等恢复；有在途 → 落入收分支排空（不调 WaitWhilePaused，
        // 收分支不阻塞 — 否则 c_line 永不释放）。
        if (paused_.load() && !paper_active_.load() && !paper_pending && c_line.empty() &&
            next_ < spans.size()) {
            if (!WaitWhilePaused()) {
                if (last_error_.empty())
                    last_error_ = "aborted";
                return false;
            }
            continue;
        }

        // === 换纸行：c_line 已排空，走逐行模式（§4.3 第一条）===
        if (paper_pending && c_line.empty()) {
            paper_pending = false;
            const std::string line(LineAt(spans[next_]));
            paper_active_.store(true);
            SetState("paper_change");

            int retries = 0;
            while (true) {
                // 行已解析但尚未写入时也可能收到 pause；在这里等待而不是忙等重试。
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
                        stream_disconnected_ =
                            !pipe.IsConnected() ||
                            pipe.GetConnectionSequence() != stream_connection_seq_;
                        last_error_ = stream_disconnected_ ? "转发中链路丢失" : "SendLine 失败";
                        return false;
                    }
                }

                // 换纸行 ok 可能等数十秒，分段等便于响应 abort
                WaitResult wr = WaitResult::Timeout;
                int err = -1;
                uint32_t waited = 0;
                const uint32_t slice = 1000;
                while (waited < kPaperOkTimeoutMs) {
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
                        last_error_ = "等待换纸 ok 时链路丢失";
                        return false;
                    }
                    if (pipe.GetGrblState() == GrblState::Alarm) {
                        last_error_ =
                            "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
                        return false;
                    }
                    uint32_t step =
                        (kPaperOkTimeoutMs - waited > slice) ? slice : (kPaperOkTimeoutMs - waited);
                    wr = pipe.TakeResponse(step, &err);
                    if (wr != WaitResult::Timeout) {
                        break;
                    }
                    waited += step;
                }

                if (wr == WaitResult::Ok) {
                    ++lines_sent_;
                    paper_active_.store(false);
                    // 换纸期间用户可能已按暂停：别把 paused 覆盖成 streaming。
                    SetState(paused_.load() ? "paused" : "streaming");
                    UpdateDisplayProgress();
                    if (abort_requested_.load()) {
                        last_error_ = "aborted";
                        return false;
                    }
                    break;
                }
                if (wr == WaitResult::Deferred) {
                    // error:8：暂停后重发本行（§4.3：只在逐行模式可定位重发）
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
                ESP_LOGE(TAG, "换纸行等 ok 超时: %s", line.c_str());
                last_error_ = "换纸行等 ok 超时";
                return false;
            }

            ++next_;
            continue;  // 换纸行完成，继续窗口化
        }

        // === 灌分支 ===
        if (!paper_pending && next_ < spans.size() && !paused_.load()) {
            const std::string line(LineAt(spans[next_]));

            if (LooksLikePaperLine(line)) {
                // 标记 paper_pending：下一轮起收分支会排空 c_line
                paper_pending = true;
                if (c_line.empty())
                    continue;  // c_line 已空，下一轮直接走逐行模式
                // 否则落入收分支排空
            } else {
                // 普通行窗口化灌：一行一个 Grbl 应答。不要把多行合并成同一 TCP 包；
                // 当前商业固件链路实测会出现少应答，窗口计数会永久漂移。
                const size_t need = line.size() + 1;  // R1：含换行符
                if (c_line_bytes_sum + need < kWindow || c_line.empty()) {
                    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
                    if (abort_requested_.load()) {
                        last_error_ = "aborted";
                        return false;
                    }
                    if (paused_.load()) {
                        continue;
                    }
                    if (!pipe.SendLine(line)) {
                        // SendRawLocked 已半关 socket；即使接收泵尚未来得及更新原子状态，
                        // 也必须按断连恢复，不能复用可能残留半行的 session。
                        stream_disconnected_ = true;
                        last_error_ = "转发中链路丢失";
                        return false;
                    }
                    c_line.push_back(need);
                    c_line_bytes_sum += need;
                    ++next_;
                    continue;
                }
                // 窗口满 → 落入收分支
            }
        }

        // === 收分支（R2：ok/error 都释放窗口）===
        if (c_line.empty()) {
            // 无在途且无法灌（暂停/paper_pending 排空后逐行尚未进入/全部发完）
            if (next_ >= spans.size() && !paper_pending)
                break;  // R4 收尾完成
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 按队首行类型选 timeout（§4.4：超时含义是「最老那条等了 timeout」）
        // c_line 队首对应 spans[lines_sent_]：FIFO + 每次 pop 即 ++lines_sent_，
        // 换纸行不在 c_line 但同样 ++lines_sent_，索引始终对齐。
        std::string_view front_sv = LineAt(spans[lines_sent_]);
        uint32_t timeout = kOkTimeoutMs;
        {
            std::string front_line(front_sv);
            if (LooksLikePlannerSyncLine(front_line))
                timeout = kPlannerSyncOkTimeoutMs;
            else if (LooksLikeMotionLine(front_line))
                timeout = kMotionOkTimeoutMs;
        }

        // 分段等，便于响应 abort/ALARM/链路丢失
        WaitResult wr = WaitResult::Timeout;
        int err = -1;
        uint32_t waited = 0;
        const uint32_t slice = 1000;
        while (waited < timeout) {
            if (abort_requested_.load() && !paper_active_.load()) {
                last_error_ = "aborted";
                return false;
            }
            if (pipe.GetGrblState() == GrblState::Alarm) {
                last_error_ = "写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
                return false;
            }
            if (!pipe.IsConnected() || !pipe.IsReady() ||
                pipe.GetConnectionSequence() != stream_connection_seq_) {
                stream_disconnected_ = true;
                last_error_ = "等待应答时链路丢失";
                return false;
            }
            uint32_t step = (timeout - waited > slice) ? slice : (timeout - waited);
            wr = pipe.TakeResponse(step, &err);
            if (wr != WaitResult::Timeout) {
                break;
            }
            waited += step;
        }

        if (wr == WaitResult::Ok) {
            c_line_bytes_sum -= c_line.front();
            c_line.pop_front();
            ++lines_sent_;

            UpdateDisplayProgress();
            // 进度推送用 lines_sent_（已确认数），不是 next_（已发数）——已发≠已画
            TickType_t now = xTaskGetTickCount();
            if ((now - last_notify_tick) >= pdMS_TO_TICKS(5000)) {
                last_notify_tick = now;
                float mx, my, mz;
                pipe.GetMachinePos(mx, my, mz);
                int pct = lines_total_ > 0 ? static_cast<int>(lines_sent_ * 100 / lines_total_) : 0;
                char buf[128];
                snprintf(buf, sizeof(buf), "出图进度: %d%% (%zu/%zu行) 位置 X=%.1f Y=%.1f", pct,
                         lines_sent_, lines_total_, mx, my);
                Notify(buf);
            }
        } else if (wr == WaitResult::Failed) {
            // error 同样对应并释放一条在途响应，但当前任务必须 fail closed，不能把
            // 被拒的 Z5/XY 当成已完成继续到 M30/done。
            last_error_ = "error:" + std::to_string(err);
            return false;
        } else if (wr == WaitResult::Deferred) {
            // §4.3：窗口化普通行不应收到 error:8（换纸期禁发 G0–G3，普通行不会撞
            // error:8）。若意外收到，按 Failed 处理并记日志（不应发生）。
            ESP_LOGW(TAG, "窗口化分支意外收到 error:8（err=%d），按失败处理", err);
            last_error_ = "error:8（窗口化不应发生）";
            return false;
        } else {
            // Timeout：Grbl WebUI Telnet 输出无 TX 缓冲，`ok`（loopTask）与 `?` 状态报告
            // （clientCheckTask）同核同优先级无锁并发写同一 socket，偶发部分写被抢占会
            // 静默吞掉一个 `ok`（既不推进也不报 error）。fail 前先用一份新状态报告兜底：
            // 若 Grbl 已 Idle 且 MPos 到达在途批次末行 X/Y 目标，则这批在途行确已执行完，
            // 整窗释放继续；否则仍 fail closed（限次数，避免真卡死/真丢行被无限掩盖）。
            if (ok_fallback_count < kMaxOkFallback &&
                ConfirmInFlightDoneByStatus(spans, lines_sent_, lines_sent_ + c_line.size())) {
                ++ok_fallback_count;
                const size_t released = c_line.size();
                ESP_LOGW(TAG,
                         "等 ok 超时但状态报告确认在途 %zu 行已执行完（Idle+到位），靠状态"
                         "兜底释放整窗继续（第 %d/%d 次）",
                         released, ok_fallback_count, kMaxOkFallback);
                lines_sent_ += released;
                c_line.clear();
                c_line_bytes_sum = 0;
                UpdateDisplayProgress();
                continue;
            }
            // §4.4：日志带 c_line 队首对应行内容，否则排障时行号是错的。
            ESP_LOGE(TAG, "等 ok 超时，队首在途行 [%zu]: %.*s", lines_sent_, (int)front_sv.size(),
                     front_sv.data());
            pipe.SendRealtime('?');
            last_error_ = "等 ok 超时";
            return false;
        }
    }

    return true;
}

}  // namespace hutuji
