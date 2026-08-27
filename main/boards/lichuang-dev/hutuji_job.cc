#include "hutuji_job.h"

#include "hutuji_pipe.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "http.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

#include <deque>
#include <string>

#define TAG "HutujiJob"

namespace hutuji {

namespace {
constexpr size_t kMaxGcodeBytes = 512 * 1024;
constexpr size_t kMaxPreviewBytes = 512 * 1024;
constexpr uint32_t kOkTimeoutMs = 60000;
constexpr uint32_t kPaperOkTimeoutMs = 90000;
constexpr uint32_t kMotionOkTimeoutMs = 30000;
constexpr uint32_t kJobIdleTimeoutMs = 30 * 60 * 1000;
// Grbl fork 中 M3/M5 笔控会同步 planner 后才返回 ok。页尾 M5 可能排在整页
// 运动队列之后，等待时间等同剩余绘图时间，不能按普通 M-code 的 60s 判超时。
constexpr uint32_t kPlannerSyncOkTimeoutMs = kJobIdleTimeoutMs;
constexpr uint32_t kPostPaperIdleTimeoutMs = 5000;
constexpr uint32_t kHomeIdleTimeoutMs = 30 * 1000;
// R20-JOB-01：归位行 ok 预算独立命名，不与换纸状态查询常量耦合。
constexpr uint32_t kHomeOkTimeoutMs = 5000;
constexpr uint32_t kPenOriginIdleTimeoutMs = 5000;
// 等 ok 超时后的状态兜底探测：只要一份新状态报告，2s 足够（`?` 是实时命令，
// 不排 planner 队列）。
constexpr uint32_t kOkFallbackIdleTimeoutMs = 2000;
// 交互点动前的新鲜坐标预算：与流式兜底分开，因为它落在用户连点的路径上。
// 2026-08-20 HIL 实测：MQTT goodbye 后 WiFi 重设 LOW_POWER(MAX_MODEM) 叠加
// block-ack 拆链重建（`wifi:[ADDBA] RX DELBA, reason:39`）时，Telnet 往返从常
// 态 20~70ms 退化到 1440ms（`?` 回包）与 3200ms（`ok`），2s 预算下连点第二下必
// 假失败「点动前未取到新鲜坐标」。6s 覆盖实测最坏 3.2s 仍有裕量；越界判定与
// 限幅不变，超时依旧 fail closed（宁可拒动，不拿旧坐标放行运动）。
constexpr uint32_t kJogFreshStateTimeoutMs = 6000;
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
// Z0 校准应答等待：固件会在作业开始前后自动换纸（M30/页尾归位/首页 G0X0Y0Z0/
// M721/实体键/BT 连接），换纸期 Grbl 主循环阻塞，排在队尾的 G92 Z0 要等换纸跑完
// 才执行并回 ok——2026-08-27 实机实锤 ok 迟到 8s，按 kPaperStatusTimeoutMs=5s 等
// 已把整单杀死（用户面「说完开始画，换了个纸就没了」）。预算对齐换纸行
// （kPaperOkTimeoutMs）：前 kZ0QuietWaitMs 静默（常态 ok 毫秒级），超过即按换纸
// 对待：播报 + blocking 标记保 TCP，1s 分段保持 abort 可响应。
constexpr uint32_t kZ0QuietWaitMs = 6000;
constexpr uint32_t kZ0WaitSliceMs = 1000;
constexpr int kDeferredMaxRetries = 8;
constexpr int kDisconnectReplayMaxRetries = 2;
// 暂停上限：超过则自动放弃，避免 busy_ 被无限期占用导致所有工具返回 busy。
constexpr uint32_t kMaxPauseMs = 10 * 60 * 1000;
// S3 窗口化流控窗口（§3 取值：Telnet RX ①的 43%）。应答队列容量由同一常量推导。
constexpr size_t kWindow = kStreamWindowBytes;

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
    std::string state_snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = state;
        state_snapshot = state_;
    }
    // 落笔棘轮闸清除（见 manual_pen_down_latched_ 注释）：settled/manual 之外的任何
    // 状态都意味着有 Z 接管方（出图流/笔测试/换纸/重连），此后笔位不再可信，
    // 下一次手动落笔必须重新走 G92 校准。
    if (std::strcmp(state, "idle") != 0 && std::strcmp(state, "done") != 0 &&
        std::strcmp(state, "error") != 0 && std::strcmp(state, "aborted") != 0 &&
        std::strcmp(state, "manual") != 0) {
        manual_pen_down_latched_.store(false);
    }
    if (auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay())) {
        display->UpdateMachineControlState(state_snapshot);
    }
    ESP_LOGI(TAG, "job state=%s", state_snapshot.c_str());
    // 射频 PERFORMANCE 持有：进 active 态开持有（钉 WIFI_PS_NONE），出 active 态
    // 按 app 音频通道是否仍开回落。判定走 hutuji::JobHoldsPerformance（与显示层
    // active 谓词同源）。manual 不在其中，点动窗口由 ManualTask 单独短时持有。
    if (hutuji::JobHoldsPerformance(state_snapshot.c_str())) {
        StartPerformanceHold();
    } else {
        StopPerformanceHold();
    }
}

namespace {
// app 侧是否仍需要 PERFORMANCE：音频通道开时设备处于 listening/speaking。
// Application 不直接暴露 IsAudioChannelOpened（在私有 protocol_ 上），用设备态等价。
bool AppNeedsPerformance() {
    const DeviceState s = Application::GetInstance().GetDeviceState();
    return s == kDeviceStateListening || s == kDeviceStateSpeaking;
}
}  // namespace
void Job::StartPerformanceHold() {
    if (performance_hold_active_) {
        return;
    }
    performance_hold_active_ = true;
    if (performance_timer_ == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &Job::PerformanceTimerThunk,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "hutuji_perf",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &performance_timer_) != ESP_OK) {
            performance_timer_ = nullptr;
        }
    }
    // 立即重申一次（不等首个周期），尽快退出 MAX_MODEM。
    ReassertPerformance();
    if (performance_timer_ != nullptr) {
        const uint32_t period_ms = hutuji::PerformanceReassertPeriodMs(kJogFreshStateTimeoutMs);
        esp_timer_start_periodic(performance_timer_, (uint64_t)period_ms * 1000ULL);
    }
}

void Job::StopPerformanceHold() {
    if (!performance_hold_active_) {
        return;
    }
    performance_hold_active_ = false;
    if (performance_timer_ != nullptr) {
        esp_timer_stop(performance_timer_);
    }
    // 交还控制权给 app：语音音频通道仍开时 app 需要 PERFORMANCE，否则回到 LOW_POWER
    // （与 application.cc 的就绪/音频关回调同口径）。此调用本身幂等。
    Board::GetInstance().SetPowerSaveLevel(AppNeedsPerformance() ? PowerSaveLevel::PERFORMANCE
                                                                 : PowerSaveLevel::LOW_POWER);
}

void Job::ReassertPerformance() {
    // 幂等重申：OnAudioChannelClosed 会单向把省电踩回 LOW_POWER，故持有期按周期重申。
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
}

void Job::PerformanceTimerThunk(void* arg) {
    auto* self = static_cast<Job*>(arg);
    if (self->performance_hold_active_) {
        self->ReassertPerformance();
    }
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
}

bool Job::LooksLikePaperLine(const std::string& line) {
    // 匹配现役换纸相关命令字；不解析参数语义。词边界匹配（S3-P3d）：
    // 裸前缀会把 M300 当 M30——当前校验器禁 M 码入文件，不可达，属校验器
    // 口径漂移时的防御面。
    if (HasGcodeCommandPrefix(line, "M30"))
        return true;
    if (HasGcodeCommandPrefix(line, "M721"))
        return true;
    if (HasGcodeCommandPrefix(line, "M701"))
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
        while (
            p < end && n + 1 < sizeof(buf) &&
            (std::isdigit(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-' || *p == '.')) {
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
    // R20-GW-06 更正（旧注释失实）：M3/M5 仅手工/恢复路径的笔控（Grbl
    // USE_M3_M5_AS_PEN_UP_DOWN：M3→Z=5mm 落笔 / M5→Z=0 抬笔，GCode.cpp）；
    // 生产 profile 纯 G1 Z 运动、文件不含任何 M 码（protocol.md §5 允许列表），
    // 本分支对生产文件不可达，保留作超时分类的防御面。Grbl 侧会等前序 planner
    // 执行到该笔控点后才回 ok，因此这类行的等待窗口必须覆盖剩余绘图时间。
    if ((line[1] == '3' || line[1] == '5') &&
        (line.size() == 2 || !std::isdigit(static_cast<unsigned char>(line[2])))) {
        return true;
    }
    return false;
}

std::string Job::StartDraw(const std::string& url, const std::string& preview_url) {
    if (!IsValidDrawCapabilityUrl(url, ".gcode") ||
        !IsValidDrawCapabilityUrl(preview_url, ".png")) {
        return "{\"error\":\"url/preview_url 必须是同站能力地址且扩展名正确\"}";
    }
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
    if (busy_.exchange(true)) {
        // P1-3 同参数幂等重入：服务端链式调用在生成完成点即发，云端第二步以同
        // url/preview_url 重入时回 previewing（等价「你要的这张已在预览/等确认」），
        // 避免用户看到预览却听到「写字机正忙」。判据必须是「已在预览流程中」——
        // awaiting_confirmation_ 要等 PNG 下载完才置 true，而第二步恰好落在下载窗口
        // （fixup 2026-08-24：原判据在主路径不成立）。参数不同或不在预览流程才 busy。
        std::string state;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            state = state_;
        }
        const bool in_preview_flow = state == "previewing" || state == "awaiting_confirmation";
        if (IsDuplicatePreviewReentry(in_preview_flow,
                                      url_ == url && preview_url_ == preview_url)) {
            return JsonString("previewing");
        }
        return JsonString("写字机正忙，请稍候再试");
    }
    if (!ResetAbortResetState()) {
        busy_.store(false);
        return "{\"error\":\"上一 reset owner 尚未收敛\"}";
    }
    stream_quiescence_.store(StreamQuiescence::Idle, std::memory_order_release);
    abort_hold_confirmed_.store(false);
    abort_requested_.store(false);
    awaiting_confirmation_.store(false);
    paused_.store(false);
    paper_active_.store(false);
    repeat_mode_.store(false);
    buffer_replayable_.store(false);
    url_ = url;
    preview_url_ = preview_url;
    last_error_.clear();
    // 新任务作废旧预取：上一个任务的预取可能仍在跑，epoch 推进后其发布会被拒绝。
    CancelPrefetch();
    SetState("previewing");
    BaseType_t ok = xTaskCreate(PreviewTaskEntry, "hutuji_preview", 6144, this, 4, nullptr);
    if (ok != pdTRUE) {
        busy_.store(false);
        SetState("idle");
        return "{\"error\":\"无法创建预览任务\"}";
    }
    return JsonString("previewing");
}

std::string Job::RequestConfirm() {
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
    if (!busy_.load() || !awaiting_confirmation_.load()) {
        return "{\"error\":\"当前没有待确认的预览\"}";
    }
    auto& pipe = Pipe::GetInstance();
    if (!pipe.IsConnected() || !pipe.IsReady()) {
        return "{\"error\":\"写字机未连接或未就绪\"}";
    }
    awaiting_confirmation_.store(false);
    SetState("downloading");
    // 灌流可持续数分钟，优先级必须低于 AFE 与编解码，避免挤占语音处理。
    BaseType_t ok = xTaskCreate(TaskEntry, "hutuji_draw", 8192, this, 1, nullptr);
    if (ok != pdTRUE) {
        // 回滚必须保留屏上预览：否则用户看到空屏却仍处于待确认。
        awaiting_confirmation_.store(true);
        SetState("awaiting_confirmation");
        return "{\"error\":\"无法创建出图任务\"}";
    }
    ClearPreview();
    return JsonString("started");
}

std::string Job::RequestAbort() {
    bool pen_test_active = false;
    bool paper_active = false;
    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (!busy_.load()) {
            return JsonString("ok");
        }
        if (awaiting_confirmation_.exchange(false)) {
            ClearPreview();
            prefetch_cancel_.store(true);
            abort_requested_.store(false);
            busy_.store(false, std::memory_order_release);
            SetState("aborted");
            return JsonString("预览已取消");
        }
        abort_requested_.store(true);
        prefetch_cancel_.store(true);
        stream_control_epoch_.store(
            NextStreamControlEpoch(stream_control_epoch_.load(std::memory_order_relaxed)),
            std::memory_order_release);
        pen_test_active = pen_test_active_.load();
        paper_active = paper_active_.load();
    }
    if (pen_test_active) {
        return JsonString("试笔即将停止");
    }
    std::string state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state = state_;
    }
    // 预览下载中取消：机械从未动过，绝不能发 `!`/0x18 打扰 Grbl。
    // 预览任务自己轮询 abort_requested_ 后撤图并释放 busy_。
    if (state == "previewing") {
        return JsonString("预览取消中");
    }
    if (state == "downloading" || state == "verifying") {
        return JsonString("ok");
    }
    if (paper_active) {
        Notify("写字机正在换纸，无法即停；换纸完成后任务将停止");
        return JsonString("换纸中无法即停，完成后即停");
    }
    {
        // owner 抢占与 Run 的 busy_ 释放共用 stream_mutex_，避免任务收尾后迟到创建 reset。
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (!busy_.load()) {
            return JsonString("ok");
        }
        if (pen_test_active_.load()) {
            return JsonString("试笔即将停止");
        }
        if (paper_active_.load()) {
            Notify("写字机正在换纸，无法即停；换纸完成后任务将停止");
            return JsonString("换纸中无法即停，完成后即停");
        }
        if (!StartAbortResetTask()) {
            abort_requested_.store(false);
            // 回滚同样动纪元：旧候选至多被冤枉拒发一次重试，不会漏看后续提交。
            stream_control_epoch_.store(
                NextStreamControlEpoch(stream_control_epoch_.load(std::memory_order_relaxed)),
                std::memory_order_release);
            return "{\"error\":\"无法创建 abort 恢复任务\"}";
        }
    }
    return JsonString("ok");
}

bool Job::StartAbortResetTask() {
    if (!abort_reset_owner_.TryClaim()) {
        return true;
    }
    abort_reset_worker_active_.store(true, std::memory_order_release);
    BaseType_t created = xTaskCreate(
        [](void*) {
            auto& job = Job::GetInstance();
            job.PerformAbortReset(true, true, false);
            job.abort_reset_worker_active_.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
        },
        // 4096：PerformAbortReset 的 drain 内可达深调用链
        // （ParseStatusReport→NotifyCloud→cJSON+SendMcpMessage），3072 无高水位
        // 实测余量；下次上机用 uxTaskGetStackHighWaterMark 取证后再议收紧。
        "hutuji_abort", 4096, nullptr, 6, nullptr);
    if (created != pdTRUE) {
        abort_reset_worker_active_.store(false, std::memory_order_release);
        abort_reset_owner_.CancelClaim();
        return false;
    }
    return true;
}

bool Job::PerformAbortReset(bool wait_for_stream_quiescence, bool owner_claimed,
                            bool allow_unready_reconnect) {
    auto& pipe = Pipe::GetInstance();
    if (!owner_claimed && !abort_reset_owner_.TryClaim()) {
        return WaitForAbortReset();
    }

    // R21-F04 残余：`!` 触发的 Hold 转移播报是「取消中」而非用户暂停；抑制窗
    // RAII 覆盖本函数全程（含所有 break 早退）。pause 的 Hold 播报不受影响——
    // pause 不进 PerformAbortReset。
    struct TransitionNotifyGuard {
        Pipe& p;
        explicit TransitionNotifyGuard(Pipe& pipe) : p(pipe) {
            p.SetTransitionNotifySuppressed(true);
        }
        ~TransitionNotifyGuard() { p.SetTransitionNotifySuppressed(false); }
    } transition_notify_guard{pipe};

    const uint32_t session = pipe.GetConnectionSequence();
    abort_reset_session_.store(session, std::memory_order_release);
    const TickType_t began = xTaskGetTickCount();
    bool success = false;
    do {
        // `!` 必须先于任何可能长达数十分钟的 planner-sync 应答等待。它只是
        // 安全停机字符，不是 reset；即使旧流已 Failed 也要先尽力停住机器。
        {
            std::lock_guard<std::mutex> stream_lock(stream_mutex_);
            if (!pipe.IsResetSessionReady(session, allow_unready_reconnect) ||
                !pipe.SendRealtime('!')) {
                break;
            }
        }

        bool stopped = false;
        uint32_t stopped_status_baseline = pipe.GetStatusReportSequence();
        while ((xTaskGetTickCount() - began) < pdMS_TO_TICKS(kResetRecoveryTimeoutMs)) {
            if (!pipe.IsConnected() || pipe.GetConnectionSequence() != session) {
                break;
            }
            const uint32_t before_query = pipe.GetStatusReportSequence();
            if (!pipe.SendRealtime('?')) {
                break;
            }
            const TickType_t query_began = xTaskGetTickCount();
            while (pipe.GetStatusReportSequence() == before_query &&
                   (xTaskGetTickCount() - query_began) < pdMS_TO_TICKS(1000)) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (pipe.HasFreshStoppedStatus(before_query, session)) {
                stopped_status_baseline = before_query;
                stopped = true;
                abort_hold_confirmed_.store(true, std::memory_order_release);
                break;
            }
        }
        if (!stopped) {
            break;
        }

        if (wait_for_stream_quiescence) {
            while (!CanResetAfterStream(stream_quiescence_.load(std::memory_order_acquire)) &&
                   (xTaskGetTickCount() - began) < pdMS_TO_TICKS(kResetRecoveryTimeoutMs)) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        uint32_t paper_before = 0;
        {
            std::lock_guard<std::mutex> stream_lock(stream_mutex_);
            if (!CanResetAfterStream(stream_quiescence_.load(std::memory_order_acquire)) ||
                !pipe.IsResetSessionReady(session, allow_unready_reconnect)) {
                break;
            }
            paper_before = pipe.GetPaperStatusSequence();
            if (!pipe.SendLine("[ESP901]")) {
                break;
            }
        }
        int paper_error = -1;
        if (pipe.WaitResponse(kPaperStatusTimeoutMs, nullptr, &paper_error) != WaitResult::Ok ||
            pipe.GetPaperStatusSequence() == paper_before ||
            pipe.GetPaperChangingState() != PaperChangingState::Off) {
            break;
        }
        const uint32_t banner_before = pipe.GetResetBannerSequence();
        const uint32_t post_reset_status = pipe.GetStatusReportSequence();
        if (!pipe.SendAbortReset(session, stopped_status_baseline, paper_before, banner_before,
                                 allow_unready_reconnect)) {
            break;
        }
        uint32_t idle_query_baseline = post_reset_status;
        bool queried_idle = false;
        while ((xTaskGetTickCount() - began) < pdMS_TO_TICKS(kResetRecoveryTimeoutMs)) {
            if (!pipe.IsConnected() || pipe.GetConnectionSequence() != session) {
                break;
            }
            const uint32_t banner_generation = pipe.GetResetBannerSequence();
            if (banner_generation == AbortResetToken::NextGeneration(banner_before) &&
                pipe.IsReady() && pipe.IsAuthorized()) {
                if (!queried_idle) {
                    idle_query_baseline = pipe.GetStatusReportSequence();
                    queried_idle = true;
                    if (!pipe.SendRealtime('?')) {
                        break;
                    }
                }
                if (pipe.GetStatusReportSequence() != idle_query_baseline &&
                    pipe.GetStatusReportSequence() != post_reset_status &&
                    pipe.GetGrblState() == GrblState::Idle) {
                    success = true;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } while (false);

    if (!success) {
        pipe.CancelAbortReset();
        // 不解锁未知 Alarm。若普通 abort 未能完成受限 reset，只拆会话阻止继续写；
        // busy 在 owner 终态发布后才释放，下一次连接仍须重新验明。
        if (pipe.GetGrblState() != GrblState::Alarm) {
            pipe.ShutdownSocket(session);
        }
    }
    if (!abort_reset_owner_.Complete(success)) {
        pipe.CancelAbortReset();
        return false;
    }
    return success;
}

bool Job::WaitForAbortReset() {
    const TickType_t began = xTaskGetTickCount();
    bool teardown_sent = false;
    while (abort_reset_owner_.Running() ||
           abort_reset_worker_active_.load(std::memory_order_acquire)) {
        if (!teardown_sent &&
            (xTaskGetTickCount() - began) >= pdMS_TO_TICKS(kResetRecoveryTimeoutMs)) {
            // 超预算即拆 session 令有界等待尽快失败；只有 worker 能发布 terminal phase。
            Pipe::GetInstance().ShutdownSocket(
                abort_reset_session_.load(std::memory_order_acquire));
            teardown_sent = true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return abort_reset_owner_.Phase() == AbortResetOwnerPhase::Succeeded;
}
bool Job::ResetAbortResetState() { return abort_reset_owner_.ResetIfSettled(); }

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
    // 纪元与暂停状态同锁发布；`!` 发送失败回滚时也递增一次——只冤枉当前候选行重试。
    stream_control_epoch_.store(
        NextStreamControlEpoch(stream_control_epoch_.load(std::memory_order_relaxed)),
        std::memory_order_release);
    // `!` 进给保持：Grbl 减速停住，planner 内容保留，`~` 可原地续跑。
    // 发送失败时回滚本地状态，不能把断链说成已暂停。
    if (!Pipe::GetInstance().SendRealtime('!')) {
        paused_.store(false);
        stream_control_epoch_.store(
            NextStreamControlEpoch(stream_control_epoch_.load(std::memory_order_relaxed)),
            std::memory_order_release);
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
        // 恢复失败的 paused 回滚与 pause/abort 提交同样推进纪元，拒绝旧候选行。
        stream_control_epoch_.store(
            NextStreamControlEpoch(stream_control_epoch_.load(std::memory_order_relaxed)),
            std::memory_order_release);
        return "{\"error\":\"写字机 Telnet 已断开，无法继续\"}";
    }
    SetState("streaming");
    if (auto* d = Board::GetInstance().GetDisplay())
        d->SetStatus("继续画...");
    return JsonString("ok");
}

std::string Job::RequestRepeat() {
    std::lock_guard<std::mutex> stream_lock(stream_mutex_);
    if (busy_.exchange(true)) {
        return JsonString("写字机正忙，请稍候再试");
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
    if (!ResetAbortResetState()) {
        busy_.store(false);
        return "{\"error\":\"上一 reset owner 尚未收敛\"}";
    }
    stream_quiescence_.store(StreamQuiescence::Idle, std::memory_order_release);
    abort_hold_confirmed_.store(false);
    abort_requested_.store(false);
    paused_.store(false);
    paper_active_.store(false);
    repeat_mode_.store(replay);
    last_error_.clear();
    SetState(replay ? "streaming" : "downloading");

    // 重画也可能持续数分钟，必须与确认出图使用相同的音频让行优先级。
    BaseType_t ok = xTaskCreate(TaskEntry, "hutuji_draw", 8192, this, 1, nullptr);
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
            return JsonString("写字机正忙，请稍候再试");
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

std::string Job::RequestManualControl(const std::string& action) {
    // 动作集合即奎享面板的 S3 化映射（2026-08-18 用户实测串口序列后决策全量开放）。
    static const char* const kActions[] = {
        "pen_up",     "pen_down", "jog_x+",    "jog_x-", "jog_y+",     "jog_y-",     "home",
        "set_origin", "unlock",   "motor_off", "reset",  "jog_step_1", "jog_step_10"};
    bool known = false;
    for (const char* candidate : kActions) {
        if (action == candidate) {
            known = true;
            break;
        }
    }
    if (!known) {
        return "{\"error\":\"未知的手动控制动作\"}";
    }
    if (action == "jog_step_1" || action == "jog_step_10") {
        std::string state;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state = state_;
        }
        if (state != "idle" && state != "done" && state != "error" && state != "aborted") {
            return "{\"error\":\"当前任务状态不允许手动控制\"}";
        }
        SetJogStepMm(action == "jog_step_1" ? hutuji::kJogStepMmFine : hutuji::kJogStepMmCoarse);
        if (auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay())) {
            display->UpdateMachineControlState(state);
        }
        return JsonString("ok");
    }
    auto& pipe = Pipe::GetInstance();
    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (busy_.exchange(true)) {
            return JsonString("写字机正忙，请稍候再试");
        }
        std::string state;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state = state_;
        }
        // 手动控制仅 settled 态可用：任务进行中一切手动动作都会与在途流冲突。
        if (state != "idle" && state != "done" && state != "error" && state != "aborted") {
            busy_.store(false);
            return "{\"error\":\"当前任务状态不允许手动控制\"}";
        }
        // 落笔棘轮闸（2026-08-25）：已确认落笔且无 Z 接管方时，重复 pen_down 幂等
        // 短路——pen_down 序列先 G92 Z0 重设基准再降 5mm，对已落笔位重放会以更低位
        // 为新基准继续下压，无限位开关下可累积压坏笔/纸台。语音面触发摩擦最低，
        // 屏幕按钮同闸受益。残余风险：落笔中 ok 丢失（运动已执行但判失败）不锁存，
        // 重试会再压深——失败已播报，由人现场处置。
        if (action == "pen_down" && manual_pen_down_latched_.load()) {
            busy_.store(false);
            return JsonString("已处于落笔状态，无需重复落笔");
        }
        if (!pipe.IsConnected() || !pipe.IsReady() || !pipe.IsAuthorized()) {
            busy_.store(false);
            return "{\"error\":\"写字机未连接、未就绪或未授权\"}";
        }
        // 点动同步预检（2026-08-25 实机 HIL 实证）：越界若只在 ManualTask 里拦，
        // 工具早已回 "started"，失败只进 notify，LLM 会把拒绝播报成「已经往左挪啦」。
        // 此处与任务内用同一份新鲜坐标 + 同一个 DecideJog（同源 core，改一处即两处）
        // 先判一次，越界/坐标不可信直接以返回值回 LLM；任务内判定保留作纵深防御。
        // 阻塞上界 kJogFreshStateTimeoutMs，MCP 工具线程可接受（语音链路本就秒级）。
        if (action.rfind("jog_x", 0) == 0 || action.rfind("jog_y", 0) == 0) {
            const float pre_step = GetJogStepMm();
            const float pre_dx =
                action == "jog_x+" ? pre_step : (action == "jog_x-" ? -pre_step : 0.0f);
            const float pre_dy =
                action == "jog_y+" ? pre_step : (action == "jog_y-" ? -pre_step : 0.0f);
            StartPerformanceHold();
            const bool fresh = QueryAndWaitFreshMachineState(kJogFreshStateTimeoutMs);
            float mx = 0, my = 0, mz = 0;
            if (fresh) {
                pipe.GetMachinePos(mx, my, mz);
            }
            const hutuji::JogVerdict verdict = fresh ? hutuji::DecideJog(mx, my, pre_dx, pre_dy)
                                                     : hutuji::JogVerdict::kStalePosition;
            StopPerformanceHold();
            if (verdict != hutuji::JogVerdict::kOk) {
                busy_.store(false);
                if (verdict == hutuji::JogVerdict::kOutOfBounds) {
                    const char* dir = action == "jog_x+"   ? "右"
                                      : action == "jog_x-" ? "左"
                                      : action == "jog_y+" ? "前"
                                                           : "后";
                    const float limit =
                        pre_dx != 0 ? hutuji::kJogEnvelopeMaxXMm : hutuji::kJogEnvelopeMaxYMm;
                    char reason[128];
                    snprintf(reason, sizeof(reason),
                             "{\"error\":\"点动越界：当前位置 X%.1f Y%.1f，向%s走 %.0f "
                             "毫米会超出 %.0f 毫米行程限位，已拒绝\"}",
                             mx, my, dir, pre_step, limit);
                    return std::string(reason);
                }
                return "{\"error\":\"未能取到可信坐标，已拒绝点动以保安全，请稍后再试\"}";
            }
        }
        abort_requested_.store(false);
        pending_manual_action_ = action;
        SetState("manual");
    }
    BaseType_t created = xTaskCreate(ManualTaskEntry, "hutuji_manual", 4096, this, 4, nullptr);
    if (created != pdTRUE) {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        busy_.store(false);
        SetState("idle");
        return "{\"error\":\"无法创建手动控制任务\"}";
    }
    return JsonString("started");
}

void Job::EnsureJogStepLoaded() {
    if (jog_step_loaded_) {
        return;
    }
    Settings settings("hutuji_ui");
    const int32_t stored =
        settings.GetInt("jog_step_mm", static_cast<int32_t>(hutuji::kJogStepMmDefault));
    jog_step_mm_ = hutuji::ClampJogStepMm(static_cast<float>(stored));
    jog_step_loaded_ = true;
}

float Job::GetJogStepMm() {
    EnsureJogStepLoaded();
    return jog_step_mm_;
}

void Job::SetJogStepMm(float step) {
    jog_step_mm_ = hutuji::ClampJogStepMm(step);
    jog_step_loaded_ = true;
    Settings settings("hutuji_ui", true);
    settings.SetInt("jog_step_mm", static_cast<int32_t>(jog_step_mm_ + 0.5f));
}

void Job::ManualTaskEntry(void* arg) {
    auto* self = static_cast<Job*>(arg);
    self->ManualTask();
    vTaskDelete(nullptr);
}

void Job::ManualTask() {
    auto& pipe = Pipe::GetInstance();
    const std::string action = pending_manual_action_;
    bool ok = true;
    std::string failed_step;

    // 逐行模式应答：SendLine 发前已清残留，WaitResponse 等本行 ok/error。
    auto send_ok = [&](const char* line, const char* what) -> bool {
        if (!pipe.SendLine(line)) {
            failed_step = what;
            return false;
        }
        int error_code = -1;
        const WaitResult wr = pipe.WaitResponse(kHomeOkTimeoutMs, nullptr, &error_code);
        if (wr != WaitResult::Ok) {
            failed_step = what;
            if (error_code >= 0) {
                failed_step += " (error:" + std::to_string(error_code) + ")";
            }
            return false;
        }
        return true;
    };

    if (action == "pen_up") {
        ok = send_ok("G1G90 Z0.0F10000", "抬笔") && WaitForIdle(false, kPenOriginIdleTimeoutMs);
    } else if (action == "pen_down") {
        // 对齐奎享实测落笔序列：先 G92 Z0 声明当前位，再 Z5 落笔。
        ok = send_ok("G92 Z0", "落笔校准") && send_ok("G1G90 Z5.0F10000", "落笔") &&
             WaitForIdle(false, kPenOriginIdleTimeoutMs);
    } else if (action == "jog_x+" || action == "jog_x-" || action == "jog_y+" ||
               action == "jog_y-") {
        const float step = GetJogStepMm();
        const float dx = action == "jog_x+" ? step : (action == "jog_x-" ? -step : 0.0f);
        const float dy = action == "jog_y+" ? step : (action == "jog_y-" ? -step : 0.0f);
        // 机器无限位开关，新鲜 MPos + 越界判定是点动的唯一防线（限幅与云端 §5 同源）。
        // 缓存坐标先经 `?` 新鲜读确认：$10=WPos/断连期的旧坐标不得放行运动（fail closed）。
        float mx = 0, my = 0, mz = 0;
        // 点动新鲜坐标正是 LOW_POWER 抖动的实测受害者（音频已关、机器 idle）。短时
        // 持有 PERFORMANCE 覆盖「取坐标 → 判界 → 发 $J → 等 Idle」整段；窗口几百 ms~
        // 几秒，无需周期重申，背光被拉亮这一下属可接受代价。出窗口立即按 app 态回落。
        StartPerformanceHold();
        if (!QueryAndWaitFreshMachineState(kJogFreshStateTimeoutMs)) {
            ok = false;
            failed_step = "点动前未取到新鲜坐标";
        } else {
            pipe.GetMachinePos(mx, my, mz);
            if (hutuji::DecideJog(mx, my, dx, dy) != hutuji::JogVerdict::kOk) {
                ok = false;
                failed_step = "点动越界";
            } else {
                // 格式逐字对齐奎享 `$J=G21G91X…Y…Z0.0F8000.0`；步进由 1/10mm 档决定。
                char line[48];
                snprintf(line, sizeof(line), "$J=G21G91X%.1fY%.1fZ0.0F8000.0", dx, dy);
                ok = send_ok(line, "点动") && WaitForIdle(false, kPenOriginIdleTimeoutMs);
            }
        }
        StopPerformanceHold();
    } else if (action == "home") {
        // G1 落 (0,0) 不触发换纸（页尾归位同款语义）；homing 指令与 G0 一律禁止——
        // 无限位时 homing 全速撞死点，G0 X0Y0 会触发自动换纸。
        ok = send_ok("G1G90 X0Y0F8000", "回原点") && WaitForIdle(false, kHomeIdleTimeoutMs);
    } else if (action == "set_origin") {
        // 逐字对齐奎享实测 `G92 X0.0 Y0.0 Z0`：把当前笔位声明为工作原点。
        ok = send_ok("G92 X0.0 Y0.0 Z0", "设置原点");
    } else if (action == "unlock") {
        ok = send_ok("$X", "解除警报");
    } else if (action == "motor_off") {
        ok = send_ok(hutuji::kMotorDisableLine, "关闭电机");
    } else if (action == "reset") {
        const uint32_t banner_before = pipe.GetResetBannerSequence();
        if (!pipe.SendRealtime(0x18)) {
            ok = false;
            failed_step = "复位";
        } else {
            const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
            while (xTaskGetTickCount() < deadline) {
                if (pipe.GetResetBannerSequence() != banner_before) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (pipe.GetResetBannerSequence() == banner_before) {
                ok = false;
                failed_step = "复位后未收到新启动横幅";
            }
        }
    }

    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        // 落笔棘轮闸锁存/解除：只在动作整体成功（含等 Idle）后更新，失败保持
        // 原值——G92 已发而 Z5 被拒的中间态靠播报让人处置，不假装知道笔位。
        if (ok && action == "pen_down") {
            manual_pen_down_latched_.store(true);
        } else if (ok && action == "pen_up") {
            manual_pen_down_latched_.store(false);
        }
        SetState("idle");
        busy_.store(false);
    }
    Notify(ok ? "手动控制完成" : "手动控制失败: " + failed_step);
}

std::string Job::StatusJson() const {
    auto& pipe = Pipe::GetInstance();
    // 发 ? 触发后台更新，不在 MCP 回调里 sleep 等待；直接返回当前缓存状态。
    if (pipe.IsConnected()) {
        pipe.SendRealtime('?');
    }
    // P1-1（协议 §4 status 分级）：本函数只读缓存——[ESP901] 是普通命令会吃 ok，
    // 发而不消费会在响应队列留下孤儿 ok，下个窗口化出图的在途计数因此漂移（实锤：
    // 窗口下队列应答即在途凭据）。遥测刷新统一走 Preview() 与出图前预检里
    // 「发 + WaitResponse 消费」的路径，status 永远只报最近一次已消费应答的值。

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
    // P1-1：遥测三字段 + Changing 态随 status 上云（值来自最近一次 [ESP901] 应答解析）。
    cJSON_AddStringToObject(root, "paper", PaperPresentStateName(pipe.GetPaperPresentState()));
    cJSON_AddStringToObject(root, "motor_en", MotorEnStateName(pipe.GetMotorEnState()));
    cJSON_AddStringToObject(root, "panel_hold", PanelHoldStateName(pipe.GetPanelHoldState()));
    cJSON_AddStringToObject(root, "paper_changing",
                            pipe.GetPaperChangingState() == PaperChangingState::On    ? "on"
                            : pipe.GetPaperChangingState() == PaperChangingState::Off ? "off"
                                                                                      : "unknown");
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

void Job::PreviewTaskEntry(void* arg) {
    static_cast<Job*>(arg)->Preview();
    vTaskDelete(nullptr);
}

void Job::Preview() {
    // 占位卡先上屏：不用等 PNG 落地，用户立刻知道「在准备预览」。
    if (auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay())) {
        display->ShowDrawPreviewLoading();
    }
    // P1-1 遥测刷新（消费式）：预览期用户最可能问「还有纸吗」——此刻必为 idle，
    // 发 [ESP901] 并在本任务内 WaitResponse 收掉 ok（普通命令的应答绝不留队，
    // 见 StatusJson 只读注释）；失败/超时静默——刷新只是尽力而为，不挡预览。
    {
        auto& pipe = Pipe::GetInstance();
        if (pipe.IsConnected() && pipe.GetPaperChangingState() != PaperChangingState::On) {
            if (pipe.SendLine("[ESP901]")) {
                int refresh_err = -1;
                (void)pipe.WaitResponse(kPaperStatusTimeoutMs, nullptr, &refresh_err);
            }
        }
    }
    if (!DownloadAndShowPreview(preview_url_)) {
        // 预览取消也走这条路（下载中收到 abort）：区分处置，避免误报错误。
        if (abort_requested_.load()) {
            ClearPreview();
            SetState("aborted");
            Notify("预览已取消");
        } else {
            ClearPreview();
            SetState("error");
            Notify("预览图加载失败，请重新生成后再试");
        }
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        abort_requested_.store(false);
        busy_.store(false, std::memory_order_release);
        return;
    }
    // 图已在屏上，但期间可能已被取消：此时必须撤图并让出 busy_，不能停在待确认。
    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (abort_requested_.load()) {
            ClearPreview();
            SetState("aborted");
            abort_requested_.store(false);
            busy_.store(false, std::memory_order_release);
            return;
        }
        awaiting_confirmation_.store(true);
        SetState("awaiting_confirmation");
    }
    Notify("预览已出来啦，喜欢就说「开始画」，不喜欢就说「取消」");
    // 预览上屏后后台预取 G-code：确认时 Run() 经 AdoptPrefetch 复用，省掉整段下载/校验等待。
    // 就地复用预览任务（栈 6144 已验证可跑 TLS），不再另建任务——2026-08-23 HIL 实测
    // xTaskCreate 在内存低谷静默失败（ minimal sram 5335），预取从未真正跑过；
    // 内联后峰值内存还省一个任务栈。取消/确认竞态与原设计一致：
    // 预取循环轮询 prefetch_cancel_/abort_requested_，AdoptPrefetch 会等 Running 收敛。
    prefetch_cancel_.store(false);
    prefetch_state_.store(PrefetchState::Running, std::memory_order_release);
    PrefetchGcode();
}

void Job::ClearPreview() {
    if (auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay())) {
        display->HideDrawPreview();
    }
}

void Job::CancelPrefetch() {
    prefetch_cancel_.store(true);
    prefetch_epoch_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    if (prefetch_buffer_ != nullptr) {
        heap_caps_free(prefetch_buffer_);
        prefetch_buffer_ = nullptr;
    }
    prefetch_len_ = 0;
    prefetch_state_.store(PrefetchState::Idle, std::memory_order_release);
}

void Job::PrefetchGcode() {
    const uint32_t epoch = prefetch_epoch_.load(std::memory_order_acquire);
    std::string url;
    {
        // url_ 由 StartDraw 在 stream_mutex_ 内写入；拷贝一份避免读到下一个任务的值。
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        url = url_;
    }
    WaitForAudioOutputIdle();
    // 预取失败永远静默回落：确认时 Run() 走正常下载，用户无感知。
    uint8_t* buf = nullptr;
    size_t len = 0;
    uint32_t crc = 0;
    bool ok = false;
    do {
        if (prefetch_cancel_.load() || abort_requested_.load()) {
            break;
        }
        auto network = Board::GetInstance().GetNetwork();
        if (!network) {
            break;
        }
        auto http = network->CreateHttp(3);
        if (!http) {
            break;
        }
        http->SetTimeout(60000);
        if (!http->Open("GET", url)) {
            break;
        }
        if (http->GetStatusCode() != 200) {
            http->Close();
            break;
        }
        const size_t content_length = http->GetBodyLength();
        if (content_length == 0 || content_length > kMaxGcodeBytes) {
            http->Close();
            break;
        }
        std::string crc_hdr = http->GetResponseHeader("X-Hutuji-CRC32");
        if (crc_hdr.empty()) {
            crc_hdr = http->GetResponseHeader("x-hutuji-crc32");
        }
        if (!ParseCrc32Header(crc_hdr, crc)) {
            http->Close();
            break;
        }
        // 与 DownloadToPsram 同策：只用 PSRAM，不饿死内部堆。
        buf = static_cast<uint8_t*>(
            heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf == nullptr) {
            http->Close();
            break;
        }
        size_t total = 0;
        while (total < content_length) {
            if (prefetch_cancel_.load() || abort_requested_.load()) {
                break;
            }
            int n = http->Read(reinterpret_cast<char*>(buf + total), content_length - total);
            if (n <= 0) {
                break;
            }
            total += static_cast<size_t>(n);
        }
        http->Close();
        if (total != content_length) {
            break;
        }
        if (!Crc32Matches(crc, Crc32Ieee(buf, total))) {
            break;
        }
        len = total;
        ok = true;
    } while (false);
    if (ok) {
        std::lock_guard<std::mutex> lock(prefetch_mutex_);
        // epoch 变了说明新任务已开始或本任务被作废：产物不得发布。
        if (!prefetch_cancel_.load() && prefetch_epoch_.load(std::memory_order_acquire) == epoch) {
            prefetch_buffer_ = buf;
            prefetch_len_ = len;
            prefetch_crc_ = crc;
            prefetch_url_ = url;
            prefetch_state_.store(PrefetchState::Ready, std::memory_order_release);
            ESP_LOGI(TAG, "G-code 预取完成 %zu 字节，确认即画", len);
            return;
        }
    }
    if (buf != nullptr) {
        heap_caps_free(buf);
    }
    if (prefetch_epoch_.load(std::memory_order_acquire) == epoch) {
        prefetch_state_.store(PrefetchState::Idle, std::memory_order_release);
    }
}

bool Job::AdoptPrefetch() {
    // 用户秒确认时预取可能仍在跑：等它收敛（通常只剩 CRC 尾段），abort 立即放行。
    while (prefetch_state_.load(std::memory_order_acquire) == PrefetchState::Running) {
        if (abort_requested_.load()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    if (prefetch_state_.load() != PrefetchState::Ready || prefetch_url_ != url_) {
        return false;
    }
    buffer_ = prefetch_buffer_;
    buffer_len_ = prefetch_len_;
    expect_crc_ = prefetch_crc_;
    prefetch_buffer_ = nullptr;
    prefetch_len_ = 0;
    prefetch_state_.store(PrefetchState::Idle, std::memory_order_release);
    ESP_LOGI(TAG, "确认即画：复用预取 G-code %zu 字节", buffer_len_);
    return true;
}

void Job::WaitForAudioOutputIdle() {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateSpeaking) {
        return;
    }
    // 功放 + Wi-Fi 下载是无电池 VSYS 的最大组合负载；等播报结束再开始下载。
    // 等「真在播」（speaking 态）而不是 codec 的 output_enabled 标志：双工 codec
    // 监听期间为保 RX 时钟永不关输出（audio_service 刻意设计），后者让本等待恒吃满
    // 30s 上限（2026-08-23 HIL 实测预览与 G-code 两段下载各白等 30s）。
    // 上限 30s 防止异常状态死等；abort 立即放行，由下载循环的 abort 检查收敛。
    ESP_LOGI(TAG, "等待播报结束再下载（功放/WiFi 错峰）");
    for (int i = 0; i < 300 && Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
         ++i) {
        if (abort_requested_.load()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool Job::DownloadAndShowPreview(const std::string& url) {
    auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display == nullptr) {
        ESP_LOGW(TAG, "无 LVGL 显示，跳过预览");
        return false;
    }
    WaitForAudioOutputIdle();
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return false;
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        return false;
    }
    http->SetTimeout(60000);
    if (!http->Open("GET", url)) {
        return false;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "预览 HTTP status %d", http->GetStatusCode());
        http->Close();
        return false;
    }
    const size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > kMaxPreviewBytes) {
        ESP_LOGW(TAG, "预览长度非法 %u", (unsigned)content_length);
        http->Close();
        return false;
    }
    std::string crc_hdr = http->GetResponseHeader("X-Hutuji-CRC32");
    if (crc_hdr.empty()) {
        crc_hdr = http->GetResponseHeader("x-hutuji-crc32");
    }
    uint32_t expected_crc = 0;
    if (!ParseCrc32Header(crc_hdr, expected_crc)) {
        ESP_LOGW(TAG, "预览缺少或非法 X-Hutuji-CRC32");
        http->Close();
        return false;
    }
    // 与 G-code 同策：只用 PSRAM，不回落内部堆，避免饿死 WiFi/音频。
    auto* data = static_cast<uint8_t*>(
        heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        http->Close();
        return false;
    }
    size_t total = 0;
    bool aborted = false;
    while (total < content_length) {
        if (abort_requested_.load()) {
            aborted = true;
            break;
        }
        int n = http->Read(reinterpret_cast<char*>(data + total), content_length - total);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<size_t>(n);
    }
    http->Close();
    if (aborted || total != content_length || !Crc32Matches(expected_crc, Crc32Ieee(data, total))) {
        heap_caps_free(data);
        return false;
    }
    // LvglAllocatedImage 构造成功即接管 data 的所有权（析构里 heap_caps_free）；
    // 只有构造抛出（PNG 头非法）时才由本函数释放，否则就是 double free。
    std::unique_ptr<LvglAllocatedImage> image;
    try {
        image = std::make_unique<LvglAllocatedImage>(data, total);
    } catch (const std::exception& exc) {
        ESP_LOGW(TAG, "预览 PNG 解码失败: %s", exc.what());
        heap_caps_free(data);
        return false;
    }
    // 触屏按钮是主路径（绘图/嘈杂环境下语音最不可靠），语音确认仍并行有效。
    // 回调在 LVGL 任务里跑：只能投递到 Application 事件循环，不能在这里建任务或发网络。
    display->ShowDrawPreview(
        std::move(image), Lang::Strings::DRAW_PREVIEW_HINT,
        []() {
            ESP_LOGI(TAG, "ui preview action=confirm");
            Application::GetInstance().Schedule([]() { Job::GetInstance().RequestConfirm(); });
        },
        []() {
            ESP_LOGI(TAG, "ui preview action=cancel");
            Application::GetInstance().Schedule([]() { Job::GetInstance().RequestAbort(); });
        });
    return true;
}

void Job::TaskEntry(void* arg) {
    static_cast<Job*>(arg)->Run();
    vTaskDelete(nullptr);
}

void Job::Run() {
    bool ok = false;
    // R21-F01/F08：一次性播报门控与失败单出口标记随任务复位。
    paper_change_notified_ = false;
    failure_notified_ = false;
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
            Notify("开始重画");
        } else {
            SetState("downloading");
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("下载中...");
            // 预取命中则跳过整段下载/校验：确认即画；未命中回落原路径。
            const bool adopted = AdoptPrefetch();
            if (adopted) {
                if (auto* d = Board::GetInstance().GetDisplay())
                    d->SetStatus("马上开画");
            }
            if (!adopted && !DownloadToPsram(url_)) {
                if (abort_requested_.load()) {
                    SetState("aborted");
                    if (auto* d = Board::GetInstance().GetDisplay())
                        d->SetStatus("已取消");
                } else {
                    SetState("error");
                    ESP_LOGW(TAG, "下载失败: %s", last_error_.c_str());
                    // R21-F03：last_error_ 是技术诊断串（HTTP/CRC32/PSRAM），用户面
                    // 只发可行动话术；技术串留在日志与 status。
                    Notify(hutuji::DescribeTransferFailure(last_error_));
                }
                break;
            }
            if (!adopted) {
                if (abort_requested_.load()) {
                    SetState("aborted");
                    break;
                }
                SetState("verifying");
                if (auto* d = Board::GetInstance().GetDisplay())
                    d->SetStatus("校验中...");
                if (!VerifyCrc()) {
                    SetState("error");
                    ESP_LOGW(TAG, "校验失败: %s", last_error_.c_str());
                    Notify(hutuji::DescribeTransferFailure(last_error_));
                    break;
                }
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
            last_error_ = "写字机未授权，请联系卖家协助激活";
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
            if (ok || !stream_disconnected_) {
                break;
            }
            if (++disconnect_replays > kDisconnectReplayMaxRetries) {
                last_error_ = "断连自动重画次数耗尽，请检查网络后重新开始";
                break;
            }
            // 即使 abort 已到达，也必须完成重连后的 Changing 分流和安全 reset；
            // 不能让旧 planner 在 S3 已放弃任务后继续运动。
            if (!RecoverDisconnectedDraw()) {
                break;
            }
            if (abort_requested_.load()) {
                break;
            }
            ESP_LOGW(TAG, "断连恢复完成，从 PSRAM 第 1 行重画（%d/%d）", disconnect_replays,
                     kDisconnectReplayMaxRetries);
        }
        // character-counting 的 error 只丢坏行，RX/planner 中后续块仍可能继续运动。
        // StreamToGrbl 已先发 `!` 并发布 Quiesced；在发布 error/busy=false 前同步
        // 完成既有受控 reset 事务，保证软件终态与物理终态一致。
        if (stream_error_stop_required_.exchange(false, std::memory_order_acq_rel)) {
            const std::string stream_error = last_error_;
            if (!PerformAbortReset(false)) {
                last_error_ = stream_error + "；错误后受控停机失败，请断电重启";
                failure_notified_ = true;
                Notify(last_error_);
            } else {
                last_error_ = stream_error;
                ResetAbortResetState();
            }
            ok = false;
        }
        if (ok) {
            ok = ReturnHomeAfterDraw();
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
            Notify("出图完成，可以说「再来一次」直接重画");
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("画好啦！");
        } else {
            SetState("error");
            // R20-S3-04：裸 error:NN 用户读不懂；能抽出码就附中文描述（未知码保持原文）。
            // last_error_ 多为复合句（如「归位被 Grbl 拒绝 (error:110)」），取首个
            // "error:" 后的数字；ParseGrblErrorCode 要求消费到行尾，故这里直接读数。
            std::string user_error = last_error_;
            const size_t err_pos = last_error_.find("error:");
            if (err_pos != std::string::npos) {
                const char* p = last_error_.c_str() + err_pos + 6;
                if (*p >= '0' && *p <= '9') {
                    if (const char* desc = hutuji::DescribeGrblError(std::atoi(p))) {
                        user_error += "（" + std::string(desc) + "）";
                    }
                }
            }
            // R21-F07/FW-UX-01：去「转发失败:」前缀——归位/换纸/报警/断连等失败
            // 都不是「转发」，前缀误导用户以为画失败。
            // R21-F08：恢复路径已播报过的失败不二次播报（双播报实测延迟最长 90s）。
            if (!failure_notified_) {
                Notify(user_error);
            }
            if (auto* d = Board::GetInstance().GetDisplay())
                d->SetStatus("出错了");
        }
    } while (false);

    bool reset_ok = true;
    // R21-F02：abort 终态播报延到 while 循环外（stream_mutex_ 已释放、reset 结果
    // 已确定）再发；0=不播 1=已停止 2=取消失败。
    int abort_notify = 0;
    while (true) {
        std::unique_lock<std::mutex> stream_lock(stream_mutex_);
        if (abort_reset_owner_.Running() ||
            abort_reset_worker_active_.load(std::memory_order_acquire)) {
            stream_lock.unlock();
            if (!WaitForAbortReset()) {
                reset_ok = false;
            }
            continue;
        }
        if (abort_reset_owner_.Started() && !abort_reset_owner_.Succeeded()) {
            reset_ok = false;
        }
        if (!ResetAbortResetState()) {
            stream_lock.unlock();
            continue;
        }
        if (!reset_ok) {
            last_error_ = "abort reset 恢复失败";
            SetState("error");
            ok = false;
        }
        // R21-F02：do-while 内的 aborted 分支早于 reset 判定，那里说「已停止」
        // 在 reset 失败时是假话；只在这里记录，循环外发。
        if (abort_requested_.load()) {
            abort_notify = reset_ok ? 1 : 2;
        }

        // stream_mutex 同时是新任务的发布门：旧任务全部资源/状态写入必须先完成。
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
            ESP_LOGI(TAG, "任务结束 state=%s err=%s replayable=%d", state_.c_str(),
                     last_error_.c_str(), (int)buffer_replayable_.load());
        }
        Pipe::GetInstance().SetExpectBlockingPeer(false);
        Pipe::GetInstance().SetTaskSessionActive(false);
        // 必须是旧任务最后一次共享写入；解锁后 StartDraw/Repeat/PenTest 才可发布新任务。
        busy_.store(false, std::memory_order_release);
        break;
    }
    if (abort_notify == 1) {
        Notify("已停止");
    } else if (abort_notify == 2) {
        Notify("取消失败，写字机可能未停稳，请断电重启");
    }
}

bool Job::DownloadToPsram(const std::string& url) {
    WaitForAudioOutputIdle();
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
    // 生产域由 IsValidDrawUrl 强制 HTTPS；HTTP 只允许 RFC1918 联调主机。
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
    if (!ParseCrc32Header(crc_hdr, expect_crc_)) {
        last_error_ = crc_hdr.empty() ? "缺少 X-Hutuji-CRC32" : "X-Hutuji-CRC32 格式无效";
        http->Close();
        return false;
    }

    // 只用 PSRAM，不回落内部 RAM（S3-P3c）：中等文件回落可能成功但饿死内部堆，
    // 把「下载失败」换成 WiFi/音频不可预期崩溃；fail closed 更可诊断。
    buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
    if (!Crc32Matches(expect_crc_, got)) {
        last_error_ = "CRC 不符";
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
    // 灌流路径零 UI 阻塞（半墨根治，2026-08-25）：SetStatus 要拿 LVGL 显示锁，
    // 主任务渲染 Grobot 全脸动画时整帧持锁（TTS 播报期帧连帧），灌流任务在锁上
    // 停摆实测 230–640ms（证据 hub results/local-only/2026-08-25/tree-redraw-com14.log：
    // 10 次落笔期空窗，树冠/框左缘微段区无墨）→ Grbl planner 排空 → $1=25 失能
    // → 弹簧抬笔（PreparePenOrigin 注释同款设计行为）→ 后续笔画无墨。250ms 节流
    // 只降频率救不了单次锁等待，故 UI 变更按仓规走 Application::Schedule 回主任务
    // 执行：灌流任务只付互斥入队成本（µs 级），锁等待发生在主任务自身（同线程
    // LVGL，无竞争）。display 为 Board 单例所有，寿命覆盖队列延迟；text 按值捕获
    // 免悬垂；lambda 不捕 this，任务结束后迟到执行也无害。
    std::string text(buf);
    Application::GetInstance().Schedule([display, text]() { display->SetStatus(text.c_str()); });
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
        // 暂停不能无限期挂住 busy_，否则 draw/repeat/pen_test 全被顶成 busy。
        if ((xTaskGetTickCount() - began) >= pdMS_TO_TICKS(kMaxPauseMs)) {
            CommitPauseTimeoutCancel();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    // 累加本次暂停段，供 ETA 扣减（对齐奎享 g 字段）。
    paused_accum_ms_ +=
        static_cast<uint32_t>((xTaskGetTickCount() - pause_segment_start_) * portTICK_PERIOD_MS);
    return !abort_requested_.load();
}

void Job::CommitPauseTimeoutCancel() {
    // 调用点（WaitWhilePaused 与收分支冻结路径）均不持 stream_mutex_；在提交锁内
    // 同时发布 paused 回滚、abort 与纪元，避免普通灌行候选漏看超时取消。
    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        paused_.store(false);
        abort_requested_.store(true);
        stream_control_epoch_.store(
            NextStreamControlEpoch(stream_control_epoch_.load(std::memory_order_relaxed)),
            std::memory_order_release);
    }
    last_error_ = "暂停超时自动取消";
    // R20-S3-03：先启动唯一 owner，按结果分支话术。StartAbortResetTask 成功只代表
    // worker 已创建，0x18 在异步 worker 里仍可能失败——成功分支只能说「已启动」；
    // xTaskCreate 失败时 0x18 发不出，机器仍停 Hold，失败分支必须明说。
    // owner 的受控 0x18 能把 Hold 一并清掉，写字机不会卡在进给保持。
    if (!StartAbortResetTask()) {
        last_error_ = "暂停超时无法创建 abort reset 任务";
        Notify("暂停超过 10 分钟，自动取消失败，写字机可能停在暂停状态，请断电重启");
    } else {
        Notify("暂停超过 10 分钟，已启动自动取消；请确认写字机已停止");
    }
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
            // Active 与 G92 在同一提交锁下发布；abort owner 必须等本事务消费应答后 reset。
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
            } else {
                stream_quiescence_.store(StreamQuiescence::Active, std::memory_order_release);
                if (!pipe.SendLine("G92 Z0")) {
                    stream_quiescence_.store(StreamQuiescence::Failed, std::memory_order_release);
                    stream_disconnected_ = true;
                    last_error_ = "发送 Z0 校准命令失败";
                    return false;
                }
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

        struct PrepareGuard {
            std::mutex& mutex;
            std::atomic<StreamQuiescence>& state;
            bool quiesced = false;
            ~PrepareGuard() {
                std::lock_guard<std::mutex> lock(mutex);
                state.store(FinishStream(quiesced), std::memory_order_release);
            }
        } prepare_guard{stream_mutex_, stream_quiescence_};

        int error_code = -1;
        // 固件自动换纸会阻塞 Grbl 主循环，排在队尾的 G92 Z0 要等换纸跑完才执行
        // （2026-08-27 实机实锤 ok 迟到 8s，原 5s 上限先把整单杀死）。按换纸行同款
        // 预算分段等，ok 随到随画。
        WaitResult wr = WaitResult::Timeout;
        bool wait_aborted = false;
        bool paper_wait_notified = false;
        const TickType_t z0_began = xTaskGetTickCount();
        TickType_t last_wait_notify = z0_began;
        uint32_t waited = 0;
        pipe.SetExpectBlockingPeer(true);
        struct BlockingGuard {
            Pipe& p;
            ~BlockingGuard() { p.SetExpectBlockingPeer(false); }
        } blocking_guard{pipe};
        while (waited < kPaperOkTimeoutMs) {
            if (abort_requested_.load()) {
                wait_aborted = true;
                break;
            }
            if (!pipe.IsConnected() || !pipe.IsReady() ||
                pipe.GetConnectionSequence() != stream_connection_seq_) {
                break;
            }
            if (pipe.GetGrblState() == GrblState::Alarm) {
                last_error_ =
                    "Z0 校准时写字机报警 (ALARM:" + std::to_string(pipe.GetAlarmCode()) + ")";
                return false;
            }
            const uint32_t step = (kPaperOkTimeoutMs - waited > kZ0WaitSliceMs)
                                      ? kZ0WaitSliceMs
                                      : (kPaperOkTimeoutMs - waited);
            wr = pipe.WaitResponse(step, nullptr, &error_code);
            if (wr != WaitResult::Timeout) {
                break;
            }
            waited += step;
            if (!paper_wait_notified && waited >= kZ0QuietWaitMs) {
                paper_wait_notified = true;
                last_wait_notify = xTaskGetTickCount();
                // R21-F01 第三处入口：与换纸行/页尾 M30 共用 paper_change_notified_
                // 门控，整张任务只播一次换纸话术（本站点是作业起点固件自动换纸窗口）。
                if (!paper_change_notified_) {
                    paper_change_notified_ = true;
                    Notify("正在换纸，请稍候");
                    if (auto* d = Board::GetInstance().GetDisplay()) {
                        d->SetStatus("换纸中...");
                    }
                }
            } else if (paper_wait_notified &&
                       (xTaskGetTickCount() - last_wait_notify) >= pdMS_TO_TICKS(30000)) {
                last_wait_notify = xTaskGetTickCount();
                Notify("还在换纸，请稍候");
            }
        }
        if (wait_aborted) {
            last_error_ = "aborted";
            return false;
        }
        if (wr != WaitResult::Ok) {
            stream_disconnected_ = !pipe.IsConnected() || !pipe.IsReady() ||
                                   pipe.GetConnectionSequence() != stream_connection_seq_;
            last_error_ = wr == WaitResult::Timeout
                              ? "等待 Z0 校准应答超时"
                              : "Z0 校准被 Grbl 拒绝 (error:" + std::to_string(error_code) + ")";
            if (abort_hold_confirmed_.load(std::memory_order_acquire)) {
                pipe.DrainResponses();
                prepare_guard.quiesced = true;
            }
            return false;
        }
        prepare_guard.quiesced = true;
        if (paper_wait_notified) {
            ESP_LOGI(TAG, "Z0 校准应答落在固件自动换纸窗口（等待 >%lu ms），继续出图",
                     (unsigned long)waited);
        }
        ESP_LOGI(TAG, "弹簧回位后已校准 G92 Z0");
        return true;
    }

    last_error_ = "aborted";
    return false;
}

bool Job::QueryAndWaitFreshMachineState(uint32_t timeout_ms) {
    auto& pipe = Pipe::GetInstance();
    if (!pipe.IsConnected() || !pipe.IsReady()) {
        return false;
    }
    // 必须同时取得 `?` 之后的新状态与新有限 MPos；$10=WPos 或 NaN/Inf 只能推进
    // status_seq，不能让旧机器坐标冒充当前位置。
    const uint32_t status_seq = pipe.GetStatusReportSequence();
    const uint32_t mpos_seq = pipe.GetMposReportSequence();
    if (!pipe.SendRealtime('?')) {
        return false;
    }
    const TickType_t began = xTaskGetTickCount();
    while (pipe.GetStatusReportSequence() == status_seq ||
           pipe.GetMposReportSequence() == mpos_seq) {
        if ((xTaskGetTickCount() - began) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
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

    if (!QueryAndWaitFreshMachineState(kOkFallbackIdleTimeoutMs)) {
        return false;
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
    // 断连窗口开始前可能已有 abort worker 抢到 owner；必须等其真正退出并清掉
    // settled phase，否则同步 reconnect reset 会被旧 owner 拦截。此处 paper_active_=true，
    // 后续 RequestAbort 不会再创建第二个 owner；若抢占刚完成则重新取锁复核。
    while (true) {
        bool owner_started = false;
        {
            std::lock_guard<std::mutex> stream_lock(stream_mutex_);
            owner_started = abort_reset_owner_.Started();
        }
        if (!owner_started) {
            break;
        }
        (void)WaitForAbortReset();
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (ResetAbortResetState()) {
            break;
        }
    }
    if (auto* d = Board::GetInstance().GetDisplay())
        d->SetStatus("写字机重连中...");
    Notify("写字机连接中断，正在安全恢复这幅画");
    const TickType_t reconnect_began = xTaskGetTickCount();
    TickType_t last_reconnect_notify = reconnect_began;
    while (true) {
        if (pipe.IsConnected() && pipe.GetConnectionSequence() != stream_connection_seq_) {
            break;
        }
        if ((xTaskGetTickCount() - reconnect_began) >= pdMS_TO_TICKS(kReconnectReadyTimeoutMs)) {
            // R21-F12：超时话术附可执行动作。
            last_error_ = "等待写字机重连超时，请检查写字机电源和网络后重新开始";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        // R21-F06：重连等待最长 120s 且期间零状态更新；每 30s 一次进展播报。
        if ((xTaskGetTickCount() - last_reconnect_notify) >= pdMS_TO_TICKS(30000)) {
            last_reconnect_notify = xTaskGetTickCount();
            Notify("还在重连写字机，请稍候");
        }
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
        // R21-F08：本分支已播报，Run() 收尾的失败出口不得二次播报；状态同步前置
        // 为 error，避免「已停止恢复」播报与 reconnecting 状态并存（最长 90s 轮询期）。
        failure_notified_ = true;
        SetState("error");
        // 不发 reset/M30。保留会话保护并轮询到换纸退出，使 Pipe 不会在
        // Changing=On 时自动跑 G1 授权探针；任务仍以 error 结束，不会续画。
        // 换纸窗口内同样打开 blocking 标记，避免等待期间 silent-poll 误杀 TCP。
        // R11-PIPE-02（第二处）：同款 RAII——本分支唯一出口是 return false，此前
        // 完全不复位，只靠 Run() 收尾兜底；与上面的 ChangePaperAfterDraw 统一。
        pipe.SetExpectBlockingPeer(true);
        struct BlockingGuard {
            Pipe& p;
            ~BlockingGuard() { p.SetExpectBlockingPeer(false); }
        } blocking_guard{pipe};
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
    // PipeTask 已关闭旧 socket 并清空其应答；新 session 再排空一次后，才把 Failed
    // 显式转为 Quiesced。只有这个已证实断连的恢复路径能做此转换。
    pipe.DrainResponses();
    stream_quiescence_.store(StreamQuiescence::Quiesced, std::memory_order_release);

    // 普通画线断连与用户 abort 共用同一受限 reset 事务和恢复判据。
    if (!PerformAbortReset(false, false, true)) {
        last_error_ = "断连恢复 abort reset 失败";
        return false;
    }
    // 断连恢复只清 planner，不终结任务；释放已收敛 owner 供本任务后续真实 abort 使用。
    if (!ResetAbortResetState()) {
        last_error_ = "断连恢复 reset owner 未收敛";
        return false;
    }
    if (abort_requested_.load()) {
        paper_active_.store(false);
        last_error_ = "aborted";
        return true;
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

bool Job::ReturnHomeAfterDraw() {
    auto& pipe = Pipe::GetInstance();

    // 归位（2026-08-14 用户决策「画完一张之后要归位」）：画完一页先把笔架送回
    // 原点，再进换纸。必须用 G1 不能用 G0：固件「回原点后换纸」触发
    // 本行与 M30 一样是 S3 编排命令、不属于下载文件；下载文件的 X0Y0 禁令不变。
    // R20 补记三条刻意决策：
    // ①quiescence 不置 Active——入口时流已被 StreamToGrbl 的 WindowGuard 发布为
    //   Quiesced，abort 的 CanResetAfterStream 立即放行（与 ChangePaperAfterDraw
    //   的 M30 编排同款：页尾单行编排不再触碰 quiescence 标记）。
    // ②归位 error:8 不重试——换纸窗外不应出现 error:8，重试被拒运动无意义。
    // ③归位窗口（~3-5s）内断连不可恢复（任务 error、纸留机上）——窗口在重画
    //   循环之外，与 ChangePaperAfterDraw 的既有不可恢复窗口同性质，显式接受。
    // G1 落 (0,0) 不触发——归位与换纸因此仍是两个独立阶段，运动留在可即停的
    // 常规行里，不进 90s 不可即停的换纸窗口（既有 host test 钉死的口径）。
    // 本行与 M30 一样是 S3 编排命令、不属于下载文件；下载文件的 X0Y0 禁令不变。
    // 仅限正常页尾调用：断连恢复的废纸换纸（RecoverDisconnectedDraw 内
    // ChangePaperAfterDraw 调用点）不归位——该路径刚经历受限 reset，position
    // 可信度最低，只允许固定的受限恢复序列。
    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (abort_requested_.load()) {
            last_error_ = "aborted";
            return false;
        }
        if (!pipe.SendLine("G1G90 X0Y0F8000")) {
            last_error_ = "归位命令发送失败";
            return false;
        }
    }
    int err = -1;
    const WaitResult wr = pipe.WaitResponse(kHomeOkTimeoutMs, nullptr, &err);
    if (wr != WaitResult::Ok) {
        last_error_ = wr == WaitResult::Timeout
                          ? "等待归位应答超时"
                          : "归位被 Grbl 拒绝 (error:" + std::to_string(err) + ")";
        return false;
    }
    // G1 的 ok 只表示已入 planner，不代表走完；必须 fresh Idle 确认归位物理完成，
    // 才允许进入换纸——否则「归位/换纸两阶段分离」只是靠 M30 内部 synchronize 的
    // 隐性保证，abort 在两阶段之间没有真实决策点。
    if (!WaitForIdle(true, kHomeIdleTimeoutMs)) {
        if (last_error_.empty()) {
            last_error_ = "归位后未确认写字机 Idle";
        } else if (last_error_ != "aborted") {
            last_error_ = "归位后未确认写字机 Idle: " + last_error_;
        }
        return false;
    }
    ESP_LOGI(TAG, "页尾归位完成（G1 X0Y0，不触发换纸）");
    return true;
}

bool Job::ChangePaperAfterDraw() {
    auto& pipe = Pipe::GetInstance();
    // 职责边界：换纸机械、时序、运动、传感器与错误判定全部由 Grbl 的
    // user_m30()/paper_auto_change() 实现。S3 这里只发唯一页结束命令 M30，
    // 等待其最终 ok/error，并在阻塞期间保护 Telnet 会话；不得加入纸路控制步骤。

    // R11-PIPE-02：blocking 标记走 RAII，与换纸行分支的 BlockingGuard 同款。M30 等待
    // 循环的 link-lost / ALARM 两个早退覆盖不到手工复位（此前只靠 Run() 收尾兜底，
    // 属隐性契约——再插一个等待分支就是真泄漏）。复位无条件做：abort 早退时它落在
    // 一个本就为 false 的标记上，幂等无副作用，故不需要 armed 记账。
    struct BlockingGuard {
        Pipe& p;
        ~BlockingGuard() { p.SetExpectBlockingPeer(false); }
    } blocking_guard{pipe};

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
            paper_active_.store(false);
            last_error_ = "自动换纸命令发送失败";
            return false;
        }
    }
    // R21-F01（页尾）：正常页尾与断连恢复的废纸换纸共用本函数；与换纸行分支
    // 共用同一门控，整张任务只播一次。必须在 stream_mutex_ 外发——Notify 走
    // cJSON + SendMcpMessage 网络路径，锁内调用会堵死新任务发布门（F02 同口径）。
    if (!paper_change_notified_) {
        paper_change_notified_ = true;
        Notify("正在换纸，请稍候");
    }

    WaitResult wr = WaitResult::Timeout;
    int err = -1;
    uint32_t waited = 0;
    TickType_t last_wait_notify = xTaskGetTickCount();
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
        // P2-1：90s 等待只播一次会被误当卡死；每 30s 续播一次（对齐 R21-F06 重连模式）。
        if ((xTaskGetTickCount() - last_wait_notify) >= pdMS_TO_TICKS(30000)) {
            last_wait_notify = xTaskGetTickCount();
            Notify("还在换纸，请稍候");
        }
    }

    if (wr != WaitResult::Ok) {
        if (wr == WaitResult::Failed || wr == WaitResult::Deferred) {
            // R21-F07：error:90 在换纸路径 = 缺纸/卡纸（protocol §7 OUT_OF_PAPER）。
            // 通用码映射契约保持 90==nullptr（recovery_core.h 钉死）；动作建议在源头句给出。
            if (err == 90) {
                ESP_LOGE(TAG, "自动换纸失败 (error:90 缺纸/卡纸)");
                last_error_ = "纸张用完或未放好，请整理好纸张后再试";
            } else {
                last_error_ = "自动换纸失败 (error:" + std::to_string(err) + ")";
            }
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
    TickType_t last_display_tick = xTaskGetTickCount();  // 屏显节流（与播报节流分列）

    // 空坐闸（2026-08-26）：旧实现 spans 为空时直接 return true → WaitForIdle →
    // 归位 → 换纸，用户体感「开始画只换纸、笔架动都没动」。云端零运动闸只拦
    // 生成物，拦不住设备侧 buffer 空/解析空。无 XY 同行（纯 Z/模态）同样拒绝。
    size_t xy_moves = 0;
    for (const auto& sp : spans) {
        const std::string_view sv = LineAt(sp);
        float ignored = 0.0f;
        if (ExtractGcodeWord(sv, 'X', ignored) || ExtractGcodeWord(sv, 'Y', ignored)) {
            ++xy_moves;
        }
    }
    if (spans.empty() || xy_moves == 0) {
        last_error_ =
            spans.empty() ? "G-code 无有效行，拒绝空坐出图" : "G-code 无 XY 运动，拒绝空坐出图";
        ESP_LOGE(TAG, "%s (buffer_len=%zu spans=%zu)", last_error_.c_str(), buffer_len_,
                 spans.size());
        return false;
    }
    ESP_LOGI(TAG, "开始灌流 lines=%zu xy=%zu bytes=%zu", spans.size(), xy_moves, buffer_len_);

    // Idle→Active 发布与 abort 提交锁原子化；abort 已在校准/下载阶段收敛时不得再开窗。
    {
        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
        if (abort_requested_.load()) {
            stream_quiescence_.store(StreamQuiescence::Quiesced, std::memory_order_release);
            return false;
        }
        stream_quiescence_.store(StreamQuiescence::Active, std::memory_order_release);
    }
    pipe.SetWindowed(true);
    struct WindowGuard {
        Pipe& pipe_ref;
        std::mutex& mutex;
        std::atomic<StreamQuiescence>& state;
        bool quiesced = false;
        void MarkQuiesced() { quiesced = true; }
        ~WindowGuard() {
            pipe_ref.SetWindowed(false);
            std::lock_guard<std::mutex> lock(mutex);
            state.store(FinishStream(quiesced), std::memory_order_release);
        }
    } window_guard{pipe, stream_mutex_, stream_quiescence_};

    // 照抄官方 stream.py 的 c_line / g_count 结构（§3）
    std::deque<size_t> c_line;    // 在途各行字节数（含 +1 换行，R1）
    size_t c_line_bytes_sum = 0;  // 在途字节数累加
    size_t next_ = 0;             // 下一条待发

    // paper_pending：遇到换纸行时先排空 c_line，排空后在此标记下走逐行模式。
    // 理由：换纸期主循环阻塞，若在途字节涌入会撑满 InputBuffer ②导致静默丢
    // 字节（§3）；换纸行 ok 等数十秒且期间禁发 G0–G3（§3 换纸阻塞期条）。
    bool paper_pending = false;
    // 等 ok 超时后靠状态报告兜底的次数。Grbl WebUI Telnet 输出无 TX 缓冲，`ok` 与
    // `?` 报告同核并发写同一 socket，偶发被抢占会静默吞掉一个 `ok`。超上限仍失败，
    // 避免真丢行/真卡死被无限掩盖。
    int ok_fallback_count = 0;
    // R10-S3-01：收分支暂停冻结的累计暂停时长与「暂停超时已提交」标记。
    // 暂停且有在途行时送分支被跳过、WaitWhilePaused 不可用（会阻塞收响应），
    // 只能在收分支的等待循环里冻结计时；见下方 DecideRecvWaitTick 接线。
    uint32_t paused_recv_ms = 0;
    bool pause_timeout_cancel = false;

    // TODO(S4/Bf)：实机出图期周期性 ?，若 Telnet Bf 余量 < 300 则临时收窄窗口。
    //   自适应反压（§3.1）本次不做，S3 核心是 window=512 + 换纸行排空。

    while (next_ < spans.size() || !c_line.empty()) {
        if (abort_requested_.load() && !paper_active_.load() && c_line.empty()) {
            // 已停止灌新行且旧应答全部消费，reset owner 可在窗口关闭后继续。
            last_error_ = "aborted";
            window_guard.MarkQuiesced();
            return false;
        }
        // abort 且仍有在途时禁止继续灌，落入收分支把旧应答消费干净。
        const bool drain_for_abort = abort_requested_.load() && !paper_active_.load();
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

        // === 暂停（§3：停止灌新行，但必须继续收在途 ok）===
        // 暂停且无在途 → 阻塞等恢复；有在途 → 落入收分支排空（不调 WaitWhilePaused，
        // 收分支不阻塞 — 否则 c_line 永不释放）。
        if (paused_.load() && !paper_active_.load() && !paper_pending && c_line.empty() &&
            next_ < spans.size()) {
            if (!WaitWhilePaused()) {
                if (abort_requested_.load() && c_line.empty()) {
                    window_guard.MarkQuiesced();
                }
                if (last_error_.empty())
                    last_error_ = "aborted";
                return false;
            }
            continue;
        }

        // === 换纸行：c_line 已排空，走逐行模式（§3 换纸阻塞期条）===
        if (paper_pending && c_line.empty()) {
            paper_pending = false;
            // §3：换纸期 Grbl 主循环不转、不回 `?`。本分支与 ChangePaperAfterDraw
            // 同样是换纸等待，必须置 blocking 标记，否则 PipeTask 的 silent-poll
            // 7×3s≈21s 会把还在等换纸 ok（预算 90s）的 TCP 掐断，任务以「等待换纸
            // ok 时链路丢失」失败。RAII 保证任何出口（ok/重试耗尽/断连/abort）复位。
            pipe.SetExpectBlockingPeer(true);
            struct BlockingGuard {
                Pipe& p;
                ~BlockingGuard() { p.SetExpectBlockingPeer(false); }
            } blocking_guard{pipe};
            const std::string line(LineAt(spans[next_]));
            paper_active_.store(true);
            SetState("paper_change");
            // R21-F01：换纸等待最长 90s（kPaperOkTimeoutMs）且期间无 ok、无进度
            // 播报，用户会误以为卡死；入口一次性播报 + 屏显。多页文件的后续换纸行
            // 不再重复播报（门控在 Run() 开始时复位）。
            if (!paper_change_notified_) {
                paper_change_notified_ = true;
                Notify("正在换纸，请稍候");
                if (auto* d = Board::GetInstance().GetDisplay())
                    d->SetStatus("换纸中...");
            }

            int retries = 0;
            while (true) {
                // 行已解析但尚未写入时也可能收到 pause；在这里等待而不是忙等重试。
                if (!WaitWhilePaused()) {
                    if (abort_requested_.load() && c_line.empty()) {
                        window_guard.MarkQuiesced();
                    }
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

                // 换纸行 ok 可能等数十秒，分段等便于响应 abort。
                WaitResult wr = WaitResult::Timeout;
                int err = -1;
                uint32_t waited = 0;
                TickType_t last_wait_notify = xTaskGetTickCount();
                const uint32_t slice = 1000;
                while (waited < kPaperOkTimeoutMs) {
                    if (!WaitWhilePaused()) {
                        if (abort_requested_.load() && c_line.empty()) {
                            window_guard.MarkQuiesced();
                        }
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
                    const uint32_t step =
                        (kPaperOkTimeoutMs - waited > slice) ? slice : (kPaperOkTimeoutMs - waited);
                    wr = pipe.TakeResponse(step, &err);
                    if (wr != WaitResult::Timeout) {
                        break;
                    }
                    waited += step;
                    // P2-1：每 30s 续播一次换纸进展（对齐 R21-F06 重连模式）。
                    if ((xTaskGetTickCount() - last_wait_notify) >= pdMS_TO_TICKS(30000)) {
                        last_wait_notify = xTaskGetTickCount();
                        Notify("还在换纸，请稍候");
                    }
                }

                if (wr == WaitResult::Ok) {
                    ++lines_sent_;
                    paper_active_.store(false);
                    SetState(paused_.load() ? "paused" : "streaming");
                    if (abort_requested_.load()) {
                        last_error_ = "aborted";
                        window_guard.MarkQuiesced();
                        return false;
                    }
                    break;
                }
                if (wr == WaitResult::Deferred) {
                    paper_active_.store(true);
                    SetState("paper_change");
                    ++retries;
                    if (retries > kDeferredMaxRetries) {
                        ESP_LOGE(TAG, "换纸行 error:8 重试耗尽: %s", line.c_str());
                        // R21-F13：不带 error:8 原文——映射层会把「正在换纸」进行式
                        // 拼到「已停止」结论上，时态矛盾；技术细节留在上面的日志。
                        last_error_ = "换纸未完成，已停止本次绘图，请检查纸张后重试";
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
                pipe.SendRealtime('?');
                ESP_LOGE(TAG, "换纸行等 ok 超时: %s", line.c_str());
                last_error_ = "换纸行等 ok 超时";
                return false;
            }

            ++next_;
            continue;
        }

        // === 灌分支 ===
        if (!drain_for_abort && !paper_pending && next_ < spans.size() && !paused_.load()) {
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
                    // 快照与暂停/abort 提交在同一把 stream_mutex_ 下做：RequestPause/
                    // RequestAbort 在锁内发布状态并递增 stream_control_epoch_；解锁后、
                    // SendLine 前用 DecideStreamSend 复核「快照后无新提交」。
                    bool snap_abort = false;
                    bool snap_paused = false;
                    uint32_t snap_epoch = 0;
                    {
                        std::lock_guard<std::mutex> stream_lock(stream_mutex_);
                        snap_abort = abort_requested_.load(std::memory_order_acquire);
                        snap_paused = paused_.load(std::memory_order_acquire);
                        snap_epoch = stream_control_epoch_.load(std::memory_order_acquire);
                    }
                    // 发送不能持锁：SendLine 可能因 TCP 背压阻塞最坏 20s，持锁会让
                    // pause/abort 无法发布 `!`。纪元闸把「暂停提交 vs 候选行检查」线性化：
                    //   a) 快照先取（旧纪元），pause/abort 随后提交 → latest != snap → Aborted；
                    //   b) pause/abort 先在锁内提交，快照看到新纪元/新状态 → Paused/Aborted。
                    // 残余窗口：本行 Allowed 决策后、字节实际到达 Grbl 前，pause 仍可在锁内
                    // 提交并成功发出 `!`——至多一行在途普通行会进入 Hold 态 planner（协议 §4.1
                    // 承认该形态：abort 由 0x18 丢弃，pause 恢复 `~` 后至多多执行这一行）。这是
                    // `!` 快路径（背压响应快）与严格无竞态（`!` 走写锁，暂停最坏延迟 20s）之间的
                    // 权衡，比改造前「解锁即发」的窗口更窄，不再放宽。
                    switch (
                        DecideStreamSend(snap_abort, snap_paused, snap_epoch,
                                         stream_control_epoch_.load(std::memory_order_acquire))) {
                        case StreamSendCancel::Paused:
                            // `!` 已发出：本行不得再进 planner；回循环顶由暂停分支等待恢复。
                            continue;
                        case StreamSendCancel::Aborted:
                            // abort 提交方已在同一把锁内置位 abort_requested_，循环顶的排流/
                            // Quiesced 逻辑只认该标志，这里无需再写状态；取消 ≠ socket 故障，
                            // 绝不置 stream_disconnected_。回循环顶走既有收分支/abort 恢复路径。
                            continue;
                        case StreamSendCancel::Allowed:
                            break;
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

        // 按队首行类型选 timeout（§3：超时含义是「最老那条等了 timeout」）
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

        // abort owner 已用 fresh Hold:0/Idle 证明 planner 停稳：旧 ok 不再代表可续画
        // 进度，本流必须整窗弃掉并把 quiescence 判为 Quiesced，受限 reset 才发得出去。
        // 提成 lambda 是因为有两个入口（循环顶 + 循环因 waited 走满而退出），两处逻辑
        // 必须逐字一致：任一处漏 MarkQuiesced 都会让 abort 卡在自家 CanResetAfterStream。
        auto quiesce_for_abort = [&]() {
            pipe.DrainResponses();
            c_line.clear();
            c_line_bytes_sum = 0;
            // 暂停超时提交的 abort 保留原因文言，用户侧才能对上 Notify 的解释。
            if (!pause_timeout_cancel) {
                last_error_ = "aborted";
            }
            window_guard.MarkQuiesced();
        };

        auto fail_window_and_stop = [&](const std::string& error) {
            // Grbl 官方 character-counting reservation：坏行回 error 后，RX 内后续行
            // 仍会继续解析执行。先实时 hold，再清软件窗口并发布 Quiesced；Run 退出
            // StreamToGrbl 后同步走受控 reset 清 planner/RX，完成前不发布 error。
            pipe.SendRealtime('!');
            pipe.DrainResponses();
            c_line.clear();
            c_line_bytes_sum = 0;
            last_error_ = error;
            stream_error_stop_required_.store(true, std::memory_order_release);
            window_guard.MarkQuiesced();
            return false;
        };

        // 分段等，便于响应 abort/ALARM/链路丢失
        WaitResult wr = WaitResult::Timeout;
        int err = -1;
        uint32_t waited = 0;
        const uint32_t slice = 1000;
        while (waited < timeout) {
            if (abort_requested_.load() && abort_hold_confirmed_.load(std::memory_order_acquire)) {
                quiesce_for_abort();
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
            // R10-S3-01：Hold 期 Grbl 主循环阻塞在挂起自旋，在途行躺在 RX 缓冲、
            // ok 不会到来——此刻的静默不是失联，等 ok 计时必须冻结，否则
            // kMotionOkTimeoutMs=30s 一到任务按「等 ok 超时」死掉，既不发 `~` 也
            // 不 reset，写字机永久卡 Hold（只能断电解救）。冻结不是无限：暂停累计
            // 达 kMaxPauseMs 走与 WaitWhilePaused 同一条超时收敛，abort owner 的
            // 受控 0x18 清掉 Hold 后，本循环顶的排流分支接管退出。仍继续
            // TakeResponse：Hold 前已解析行的迟到 ok 与恢复 `~` 后的应答都要照收。
            switch (DecideRecvWaitTick(paused_.load(), paused_recv_ms, kMaxPauseMs)) {
                case RecvWaitTick::FreezePaused:
                    paused_recv_ms += step;
                    // 冻结时长计入 ETA 暂停扣减（与 WaitWhilePaused 同口径）。
                    paused_accum_ms_ += step;
                    continue;
                case RecvWaitTick::PauseTimedOut:
                    CommitPauseTimeoutCancel();
                    pause_timeout_cancel = true;
                    paused_recv_ms = 0;
                    continue;
                case RecvWaitTick::Accrue:
                    break;
            }
            // 暂停结束（恢复或超时取消）后重置累计，下一次暂停另起新额度。
            paused_recv_ms = 0;
            waited += step;
        }

        // R11-PIPE-01：循环退出后必须再复核一次。暂停超时提交 abort 时，在途行的
        // waited 常只差最后一个 slice：CommitPauseTimeoutCancel 后 continue，下一片
        // TakeResponse 超时返回即让 waited 走满 timeout，`while` 先判假，循环顶那道
        // 排流分支再也执行不到。落到下面的 ok 兜底必然失败（要 fresh Idle，机器在
        // Hold）且不 MarkQuiesced → Failed → abort owner 的 CanResetAfterStream 为假
        // → 受限 reset 发不出去 → 写字机滞留 Hold（只能外部 `~`/断电）、连接被拆。
        if (abort_requested_.load() && abort_hold_confirmed_.load(std::memory_order_acquire)) {
            quiesce_for_abort();
            return false;
        }

        if (wr == WaitResult::Ok) {
            c_line_bytes_sum -= c_line.front();
            c_line.pop_front();
            ++lines_sent_;

            // 进度屏显 250ms 节流：UpdateDisplayProgress 已走 Application::Schedule
            // 回主任务（见该函数注释，灌流路径零 UI 阻塞），节流保留为的是约束
            // 主任务队列流量（≤4 条/秒）。历史：2026-08-24 插桩实证每 ok 一次的
            // SetStatus 在 LVGL 锁上阻塞 ~60ms/行（68.8s/1151 行，证据
            // results/local-only/2026-08-24/hil-instr-cat-com14-v4.log 在本机），
            // 当时只节流未出灌流路径，lichuang 全脸动画下单次锁等待仍达
            // 230–640ms（2026-08-25 tree-redraw-com14.log），半墨由此复发。
            TickType_t display_now = xTaskGetTickCount();
            if ((display_now - last_display_tick) >= pdMS_TO_TICKS(250)) {
                last_display_tick = display_now;
                UpdateDisplayProgress();
            }
            // 进度推送用 lines_sent_（已确认数），不是 next_（已发数）——已发≠已画
            TickType_t now = xTaskGetTickCount();
            if ((now - last_notify_tick) >= pdMS_TO_TICKS(5000)) {
                last_notify_tick = now;
                // R21-F05：进度播报只留百分比——机器坐标/行号对最终用户是噪声
                // （坐标仍在串口逐行日志可查）。5s 节流不变。
                // 2026-08-27 半墨复发实锤：Notify 的 cJSON+MQTT/TLS 发布在灌流环内
                // 同步执行，单次数百 ms（96% 那次撞 700ms 停顿）；planner 缓冲对
                // ~2mm 碎段只顶 ~250ms，饿穿即 $1=25 失能→弹簧抬笔→整段丢墨。
                // 与 e623aec 屏显同款处理：Schedule 回主任务，灌流环只付入队成本。
                int pct = lines_total_ > 0 ? static_cast<int>(lines_sent_ * 100 / lines_total_) : 0;
                char buf[64];
                snprintf(buf, sizeof(buf), "出图进度: %d%%", pct);
                std::string text(buf);
                Application::GetInstance().Schedule([this, text]() { Notify(text); });
            }
        } else if (wr == WaitResult::Failed) {
            // error 只丢坏行；character-counting 已灌入 RX 的后续行仍会执行。
            return fail_window_and_stop("error:" + std::to_string(err));
        } else if (wr == WaitResult::Deferred) {
            // 窗口化普通行不应收到 error:8；一旦出现，后续在途同样先物理停机。
            ESP_LOGW(TAG, "窗口化分支意外收到 error:8（err=%d），停止并受控 reset", err);
            return fail_window_and_stop("error:8（窗口化不应发生）");
        } else {
            // Timeout：Grbl WebUI Telnet 输出无 TX 缓冲，`ok`（loopTask）与 `?` 状态报告
            // （clientCheckTask）同核同优先级无锁并发写同一 socket，偶发部分写被抢占会
            // 静默吞掉一个 `ok`（既不推进也不报 error）。fail 前先用一份新状态报告兜底：
            // 若 Grbl 已 Idle 且 MPos 到达在途批次末行 X/Y 目标，则这批在途行确已执行完，
            // 整窗释放继续；否则仍 fail closed（限次数，避免真卡死/真丢行被无限掩盖）。
            if (ok_fallback_count < kMaxOkFallback &&
                ConfirmInFlightDoneByStatus(spans, lines_sent_, lines_sent_ + c_line.size())) {
                ++ok_fallback_count;
                // 释放整窗前先排空应答队列：等待期内到达的迟到 ok 属于本批已释放的
                // 行，留在队列里会被下一批第一行的 TakeResponse 错配（ok 与状态行
                // 无锁并发写同一 socket，迟到 ok 是已证现象；与 abort 分支同口径）。
                pipe.DrainResponses();
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
            // 日志带 c_line 队首对应行内容，否则排障时行号是错的。
            ESP_LOGE(TAG, "等 ok 超时，队首在途行 [%zu]: %.*s", lines_sent_, (int)front_sv.size(),
                     front_sv.data());
            pipe.SendRealtime('?');
            last_error_ = "等 ok 超时";
            return false;
        }
    }

    // 双闸：解析有行但一行都没发出（异常早退漏标）同样不得装成「画完」。
    if (lines_sent_ == 0) {
        last_error_ = "灌流未发出任何行，拒绝空坐出图";
        ESP_LOGE(TAG, "%s (lines_total=%zu)", last_error_.c_str(), lines_total_);
        window_guard.MarkQuiesced();
        return false;
    }
    ESP_LOGI(TAG, "灌流完成 lines_sent=%zu/%zu", lines_sent_, lines_total_);
    return true;
}

}  // namespace hutuji
