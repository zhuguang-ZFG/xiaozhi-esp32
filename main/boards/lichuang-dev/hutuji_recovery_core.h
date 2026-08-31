#ifndef HUTUJI_RECOVERY_CORE_H
#define HUTUJI_RECOVERY_CORE_H

#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hutuji {
// 窗口按 payload+LF 计字节；应答队列须覆盖最短非空行形成的最大在途条数，
// 否则队满丢掉 error 后，后续 ok 会与错误行错配，破坏 fail-closed。
inline constexpr size_t kStreamWindowBytes = 512;
inline constexpr size_t kResponseQueueDepth = (kStreamWindowBytes - 1u) / 2u;

/**
 * 解析 Grbl 的 `error` 应答为数字错误码；无法判定时返回 -1。
 *
 * **必须同时认数字与文本两种形态。** Grbl_Esp32 `Report.cpp:236` 在
 * `$Errors/Verbose=1` 时把应答从 `error:11` 换成 `error: Line too long`：
 *
 *     if (verbose_errors->get()) grbl_sendf(client, "error: %s\r\n", errorString(status_code));
 *     else                       grbl_sendf(client, "error:%d\r\n", (int)status_code);
 *
 * 只认数字的话，后果不是降级而是**行为反转**：`error:8`（换纸期运动行被推迟、该行
 * 需重发）会解析成 -1 → 走 WaitResult::Failed → 整单失败而非重试；`error:110`
 * （未授权）同样落进 Failed 分支而非「已知未授权」。默认值是 0（`Defaults.h:82`），
 * 但这是块**现役商业固件**，`$`-设置存在 NVS 里，能被任何上位机（奎享本体、
 * ESP3D WebUI）改写并持久化，且我们无法在出厂前保证它没被动过。
 *
 * 文本表取自 `Grbl_Esp32/src/Error.cpp` 的 ErrorNames，只收录 S3 三层换纸判定与
 * 授权探测真正分派到的码 + 行长溢出；其余文本形态回 -1（与旧行为一致，按 Failed 处理）。
 */
inline int ParseGrblErrorCode(const std::string& line) {
    // "error:8" / "error:110" / 旧式 "error 8"
    if (line.rfind("error", 0) != 0) {
        return -1;
    }
    size_t i = 5;
    while (i < line.size() && (line[i] == ':' || line[i] == ' ')) {
        ++i;
    }
    if (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        int code = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            const int digit = line[i] - '0';
            if (code > (INT_MAX - digit) / 10) {
                return -1;
            }
            code = code * 10 + digit;
            ++i;
        }
        return i == line.size() ? code : -1;
    }
    if (i >= line.size()) {
        return -1;  // `error:` 后无内容：既非数字也非文本形态
    }
    // `$Errors/Verbose=1` 的文本形态。逐字对齐 Error.cpp 的 ErrorNames 字面量。
    // 注：`errorString()` 对未定义的码返回 NULL（`ProcessSettings.cpp:369`），
    // `Report.cpp:237` 会把它按 `%s` 印成 `(null)`——不在下表内，回 -1，与旧行为一致。
    const std::string text = line.substr(i);
    struct VerboseName {
        const char* text;
        int code;
    };
    static constexpr VerboseName kVerboseNames[] = {
        {"Command requires idle state", 8},  // Error::IdleError = 8，换纸期推迟
        {"Authentication failed!", 110},     // Error::AuthenticationFailed = 110
        {"Line too long", 11},               // Error::Overflow = 11
        {"GCode cannot be executed in lock or alarm state", 9},  // Error::SystemGcLock
        {"Soft limit error", 10},                                // Error::SoftLimitError
    };
    for (const VerboseName& entry : kVerboseNames) {
        if (text == entry.text) {
            return entry.code;
        }
    }
    return -1;
}

/** 只接受完整且有限的 MPos 三轴；WPos/缺轴/NaN/Inf 都不能成为完成证据。 */
inline bool ParseFiniteMPos(const std::string& status, float& x, float& y, float& z) {
    const size_t mpos = status.find("MPos:");
    if (mpos == std::string::npos) {
        return false;
    }
    float parsed_x = 0.0f, parsed_y = 0.0f, parsed_z = 0.0f;
    int consumed = 0;
    if (std::sscanf(status.c_str() + mpos, "MPos:%f,%f,%f%n", &parsed_x, &parsed_y, &parsed_z,
                    &consumed) != 3 ||
        consumed <= 0 ||
        (mpos + static_cast<size_t>(consumed) < status.size() &&
         status[mpos + static_cast<size_t>(consumed)] != '|') ||
        !std::isfinite(parsed_x) || !std::isfinite(parsed_y) || !std::isfinite(parsed_z)) {
        return false;
    }
    x = parsed_x;
    y = parsed_y;
    z = parsed_z;
    return true;
}

/**
 * 点动越界判定（2026-08-20 独立成 core）：机器无限位开关，新鲜 MPos + 本判定是
 * 点动的唯一防线；限幅与云端 protocol §5 同源（X≤190 / Y≤190mm），改云端必须
 * 同步这里。任一输入非有限数一律 fail-closed 为 kStalePosition——`$10` 可持久化
 * 成 WPos、网络/固件异常可产生 NaN/Inf，都不得冒充「已知坐标」放行运动。
 */
inline constexpr float kJogEnvelopeMaxXMm = 190.0f;
// 2026-08-21 收紧：Y 为物理宽边（210 行程），原 277 超行程。
inline constexpr float kJogEnvelopeMaxYMm = 190.0f;
/** 1mm 细步进，逐字对齐奎享实测 `$J=G21G91X1.0Y0.0Z0.0F8000.0`（R17 交叉钉）。 */
inline constexpr float kJogStepMm = 1.0f;
inline constexpr float kJogStepMmFine = 1.0f;
inline constexpr float kJogStepMmCoarse = 10.0f;
inline constexpr float kJogStepMmDefault = kJogStepMmCoarse;

/**
 * 只承认 1mm / 10mm 两档。脏 NVS、NaN、以及「看起来像中间值」一律塌到最近档：
 * 小于 5 → 1mm，其余 → 10mm。禁止把任意毫米数送进 `$J=`。
 */
inline float ClampJogStepMm(float step) {
    if (!std::isfinite(step) || step < 5.0f) {
        return kJogStepMmFine;
    }
    return kJogStepMmCoarse;
}
/**
 * 语音面（hutuji.manual）动作白名单：只开放运动/笔/步距/回原点。
 * set_origin 重写工作原点、unlock/motor_off/reset 属维护动作，误触发代价高，
 * 仅保留屏幕入口；RequestManualControl 自身的全量白名单不受影响。
 */
inline constexpr const char* kVoiceManualActions[] = {
    "pen_up", "pen_down", "jog_x+", "jog_x-", "jog_y+", "jog_y-",
    "home", "jog_step_1", "jog_step_10"};

inline bool IsVoiceAllowedAction(const std::string& action) {
    for (const char* candidate : kVoiceManualActions) {
        if (action == candidate) {
            return true;
        }
    }
    return false;
}


/**
 * 控制页状态胶囊：Grbl 态 + 三轴。坐标非有限时打 `---`，避免把 NaN 画到屏上。
 * 例：`Idle X1.0 Y2.0 Z15.0` / `Sleep X--- Y0.0 Z0.0`。
 */
inline void FormatMachineHud(char* out, size_t n, const char* grbl_state, float x, float y,
                             float z) {
    if (out == nullptr || n == 0) {
        return;
    }
    auto fmt_axis = [](char* buf, size_t cap, float v) {
        if (!std::isfinite(v)) {
            std::snprintf(buf, cap, "---");
        } else {
            std::snprintf(buf, cap, "%.1f", v);
        }
    };
    char xs[32];
    char ys[32];
    char zs[32];
    fmt_axis(xs, sizeof(xs), x);
    fmt_axis(ys, sizeof(ys), y);
    fmt_axis(zs, sizeof(zs), z);
    const char* st = (grbl_state != nullptr && grbl_state[0] != '\0') ? grbl_state : "?";
    char stbuf[12];
    std::snprintf(stbuf, sizeof(stbuf), "%s", st);
    // 宽度上限让 GCC -Wformat-truncation 看得到写入上界（胶囊本身也装不下更长串）。
    std::snprintf(out, n, "%.11s X%.7s Y%.7s Z%.7s", stbuf, xs, ys, zs);
}

/**
 * 关电机：FluidNC/Grbl_Esp32 `$MD`（Motor/Disable）。下一动自动使能。
 * 禁止 `$SLP`——Sleep 挂起期不 poll client，后续行命令只进 buffer，面板假死。
 */
inline constexpr char kMotorDisableLine[] = "$MD";

enum class JogVerdict { kOk, kStalePosition, kOutOfBounds };

inline JogVerdict DecideJog(float mx, float my, float dx, float dy) {
    if (!std::isfinite(mx) || !std::isfinite(my) || !std::isfinite(dx) || !std::isfinite(dy)) {
        return JogVerdict::kStalePosition;
    }
    if (mx + dx < 0.0f || mx + dx > kJogEnvelopeMaxXMm || my + dy < 0.0f ||
        my + dy > kJogEnvelopeMaxYMm) {
        return JogVerdict::kOutOfBounds;
    }
    return JogVerdict::kOk;
}

/**
 * 绘图机「射频 PERFORMANCE 持有」决策（2026-08-20）。
 * 症状：音频通道关闭后 application.cc:535 把 WiFi 省电踩回 LOW_POWER(MAX_MODEM,
 * listen_interval=10)，叠加 block-ack 拆链（RX DELBA reason:39）时 Telnet 往返从
 * 常态 20~70ms 退化到 1440ms（`?`）/3200ms（`ok`），点动新鲜坐标与流内 ok 兜底
 * 都因此假超时。对策是绘图/换纸/手动交互窗口把 WiFi 钉在 PERFORMANCE(WIFI_PS_NONE)，
 * 窗口结束按 app 态回落。manual 不在下表：点动的新鲜坐标窗口由调用方单独短时持有，
 * 整个 manual 态持有会让屏常亮到 settled。
 */
inline constexpr bool JobHoldsPerformance(const char* state) {
    // 与 lcd_display.cc ApplyMachineControlState 的 active 谓词同源，单独实现以免
    // 显示层与任务层互相 include；两侧任一改动须同步（已有 host 断言钉死该清单）。
    const char* const kActive[] = {
        "streaming",   "paused",    "previewing",   "awaiting_confirmation",
        "downloading", "verifying", "reconnecting", "paper_change",
        "pen_test"};
    for (const char* s : kActive) {
        const char* a = state;
        const char* b = s;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return true;
        }
    }
    return false;
}

/**
 * 点动新鲜坐标窗口（QueryAndWaitFreshMachineState）的 PERFORMANCE 重申周期。
 * 4s：比实测最坏 BA 重建 3.2s 略长即可盖住一个完整重申空窗，又不至于像 1s 那样
 * 频繁刷日志/拉亮背光。取 min(点动新鲜预算, 4000)，预算调小则同步收紧。
 */
inline constexpr uint32_t PerformanceReassertPeriodMs(uint32_t fresh_budget_ms) {
    return fresh_budget_ms < 4000u ? fresh_budget_ms : 4000u;
}

/** 生成 ZXing/Android/iOS 识别的 open SoftAP 二维码内容；不包含家庭 Wi-Fi 凭据。 */
inline std::string BuildOpenHotspotWifiQrPayload(const std::string& ssid) {
    std::string escaped;
    escaped.reserve(ssid.size() + 16);
    for (char ch : ssid) {
        if (ch == '\\' || ch == ';' || ch == ',' || ch == '"' || ch == ':') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return "WIFI:T:nopass;S:" + escaped + ";;";
}

/**
 * 用户面中文描述（R20-S3-04）：裸 `error:NN` 经 `Notify("转发失败: " + last_error_)`
 * 直达云端，用户听不懂。只对用户可行动/可理解的码给描述；未知名回 nullptr，
 * 调用方保持原文。码表与上方 kVerboseNames 同源（Grbl Error 枚举）。
 */
inline const char* DescribeGrblError(int code) {
    switch (code) {
        case 8:
            return "写字机正在换纸，暂不能执行该行";  // Error::IdleError
        case 9:
            return "写字机处于锁定/报警状态";  // Error::SystemGcLock
        case 10:
            return "超出软限位";  // Error::SoftLimitError
        case 11:
            return "指令行过长";  // Error::Overflow
        // 90（MessageFailed）不映射：它是通用码；换纸路径的报错句已自带「换纸」上下文。
        case 110:
            return "写字机未授权";  // Error::AuthenticationFailed
        default:
            return nullptr;
    }
}

/**
 * 下载/校验失败的用户面话术（R21-F03）：`last_error_` 是技术诊断串
 * （HTTP/CRC32/PSRAM/Content-Length），直接 Notify 用户听不懂也无从行动。
 * 按错误形态归类为可执行建议；技术串由调用方留 ESP 日志与 status。
 * 404 单列：TTL（默认 600s，`HUTUJI_OUTPUT_TTL_SECONDS` 可调）过期是正常路径
 * （CloudUX-F1），须引导重新生成而非重试。
 */
inline const char* DescribeTransferFailure(const std::string& error) {
    if (error.find("404") != std::string::npos) {
        return "文件已过期或不存在，请重新生成后再试";
    }
    if (error.find("CRC") != std::string::npos ||
        error.find("Content-Length") != std::string::npos ||
        error.find("512KB") != std::string::npos) {
        return "文件内容不完整，请重新生成后再试";
    }
    return "下载图片失败，请检查网络后重试";
}

/**
 * 构造未授权探测行。`G1` 必须是第一个 G 字：商业固件 Protocol.cpp 的前置授权门
 * 只检查行首 G0~G3；若写成 `G53 G1 ...`，前置门看到 G53 会放过，GCode.cpp 内层
 * 又只是静默跳过未授权运动并最终返回 ok，S3 会把未授权误判成已授权。
 */
inline std::string BuildLicenseProbeLine(float machine_x) {
    char probe[64];
    std::snprintf(probe, sizeof(probe), "G1 G53 X%.3f F1500", machine_x);
    return probe;
}

/** 服务端固定输出 8 位十六进制 CRC32；任何缺位、溢出、空白或尾缀都拒绝。 */
inline bool ParseCrc32Header(const std::string& text, uint32_t& out) {
    if (text.size() != 8) {
        return false;
    }
    uint32_t value = 0;
    for (char c : text) {
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

inline constexpr bool Crc32Matches(uint32_t expected, uint32_t actual) {
    return expected == actual;
}

/** zlib/IEEE CRC32，与 Python zlib.crc32 一致（初值 0，结果按惯例取反折叠）。
 *  下载完整性校验的单一事实源：hutuji_job 与 hutuji_music 共用。 */
inline uint32_t Crc32Ieee(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

inline bool ParseDecimalOctet(const std::string& text, size_t begin, size_t end, uint32_t& out) {
    if (begin == end || end - begin > 3) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = begin; i < end; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(c - '0');
    }
    if (value > 255u) {
        return false;
    }
    out = value;
    return true;
}

inline bool ParseIpv4(const std::string& host, uint32_t (&octets)[4]) {
    size_t begin = 0;
    for (size_t index = 0; index < 4; ++index) {
        const size_t end = index == 3 ? host.size() : host.find('.', begin);
        if (end == std::string::npos || !ParseDecimalOctet(host, begin, end, octets[index])) {
            return false;
        }
        begin = end + 1;
    }
    return begin == host.size() + 1;
}

inline bool IsDecimalPort(const std::string& text) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    uint32_t port = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        port = port * 10u + static_cast<uint32_t>(c - '0');
    }
    return port > 0u && port <= 65535u;
}

/**
 * 校验 hutuji.draw 的 capability URL。生产域只允许 HTTPS；HTTP 仅留给 RFC1918
 * 联调主机。authority 禁 userinfo/IPv6/畸形端口，路径固定为服务端的 `/files/`，
 * 且路径（query 前）必须以 expected_suffix 结尾，防 PNG/G-code 参数互换。
 */
inline bool IsValidDrawCapabilityUrl(const std::string& url, const std::string& expected_suffix) {
    constexpr const char* kHttps = "https://";
    constexpr const char* kHttp = "http://";
    for (unsigned char c : url) {
        if (c <= 0x20u || c == 0x7fu) {
            return false;
        }
    }

    bool https = false;
    size_t authority_begin = 0;
    if (url.rfind(kHttps, 0) == 0) {
        https = true;
        authority_begin = 8;
    } else if (url.rfind(kHttp, 0) == 0) {
        authority_begin = 7;
    } else {
        return false;
    }

    const size_t slash = url.find('/', authority_begin);
    if (slash == std::string::npos || slash == authority_begin ||
        url.compare(slash, 7, "/files/") != 0 || url.find('#', slash) != std::string::npos) {
        return false;
    }
    const size_t query = url.find('?', slash);
    const size_t path_end = query == std::string::npos ? url.size() : query;
    if (expected_suffix.empty() || path_end < expected_suffix.size() ||
        url.compare(path_end - expected_suffix.size(), expected_suffix.size(), expected_suffix) !=
            0) {
        return false;
    }
    const std::string authority = url.substr(authority_begin, slash - authority_begin);
    if (authority.find('@') != std::string::npos || authority.find('[') != std::string::npos ||
        authority.find(']') != std::string::npos) {
        return false;
    }

    std::string host = authority;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) {
        if (authority.find(':', colon + 1) != std::string::npos ||
            !IsDecimalPort(authority.substr(colon + 1))) {
            return false;
        }
        host = authority.substr(0, colon);
    }
    if (host.empty()) {
        return false;
    }

    std::string lower_host;
    lower_host.reserve(host.size());
    for (char c : host) {
        lower_host.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (lower_host == "hutuji.donglicao.com") {
        return https && (colon == std::string::npos ||
                         authority.compare(colon + 1, std::string::npos, "443") == 0);
    }

    uint32_t octets[4]{};
    if (!ParseIpv4(host, octets)) {
        return false;
    }
    return octets[0] == 10u || (octets[0] == 172u && octets[1] >= 16u && octets[1] <= 31u) ||
           (octets[0] == 192u && octets[1] == 168u);
}

inline bool IsValidDrawUrl(const std::string& url) {
    return IsValidDrawCapabilityUrl(url, ".gcode");
}

/**
 * 单条发送的活动时间预算。部分成功不会续期；feed hold 仲裁挂起期间不计时，避免
 * 把等待实时字符的时间误判成 TCP 背压。全部计算使用无符号差值，覆盖 tick 回绕。
 */
inline constexpr uint32_t kSendStallBudgetMs = 20000;

class SendStallBudget {
public:
    constexpr SendStallBudget(uint32_t began, uint32_t budget) : began_(began), budget_(budget) {}

    constexpr void Suspend(uint32_t from, uint32_t to) {
        suspended_ += static_cast<uint32_t>(to - from);
    }

    constexpr bool Expired(uint32_t now) const {
        const uint32_t elapsed = static_cast<uint32_t>(now - began_);
        return static_cast<uint32_t>(elapsed - suspended_) >= budget_;
    }

private:
    uint32_t began_;
    uint32_t budget_;
    uint32_t suspended_ = 0;
};

inline constexpr bool ShouldYieldToFeedHold(bool feed_hold_priority, uint32_t waiters) {
    return !feed_hold_priority && waiters > 0;
}

/** 只有 send() 真正写出正数字节时才推进游标，错误/零进展必须留在原位。 */
inline constexpr bool AdvanceSendProgress(size_t& sent, int n) {
    if (n <= 0) {
        return false;
    }
    sent += static_cast<size_t>(n);
    return true;
}

/**
 * send() 返回 EAGAIN/EWOULDBLOCK 时是否应重试（而不是立即断开）。时间预算由
 * SendStallBudget 独立判定，避免错误谓词同时拥有时钟状态。
 * @param e 当前 errno 值（errno 是宏，不能作参数名）
 */
inline constexpr bool ShouldRetrySend(int n, int e) {
    return n < 0 && (e == EAGAIN || e == EWOULDBLOCK);
}

inline constexpr bool IsStoppedForReset(bool idle, bool hold, bool hold_complete) {
    return idle || (hold && hold_complete);
}

inline constexpr bool CanSendAbortReset(bool connected, bool ready, bool same_session,
                                        bool fresh_stopped_status, bool fresh_paper_status,
                                        bool changing_off) {
    return connected && ready && same_session && fresh_stopped_status && fresh_paper_status &&
           changing_off;
}

inline constexpr bool IsResetSessionReady(bool connected, bool ready, bool task_session_active,
                                          bool allow_unready_reconnect) {
    return connected && (ready || (allow_unready_reconnect && task_session_active));
}

enum class AbortResetOwnerPhase : uint8_t {
    Idle = 0,
    Running,
    Succeeded,
    Failed,
};

class AbortResetOwner {
public:
    bool TryClaim() {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Idle;
        return phase_.compare_exchange_strong(expected, AbortResetOwnerPhase::Running,
                                              std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool CancelClaim() {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Running;
        return phase_.compare_exchange_strong(expected, AbortResetOwnerPhase::Idle,
                                              std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool Complete(bool success) {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Running;
        return phase_.compare_exchange_strong(
            expected, success ? AbortResetOwnerPhase::Succeeded : AbortResetOwnerPhase::Failed,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool FailIfRunning() { return Complete(false); }

    bool ResetIfSettled() {
        AbortResetOwnerPhase current = phase_.load(std::memory_order_acquire);
        while (current != AbortResetOwnerPhase::Running) {
            if (phase_.compare_exchange_weak(current, AbortResetOwnerPhase::Idle,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    bool Running() const {
        return phase_.load(std::memory_order_acquire) == AbortResetOwnerPhase::Running;
    }

    bool Started() const {
        return phase_.load(std::memory_order_acquire) != AbortResetOwnerPhase::Idle;
    }

    bool Succeeded() const {
        return phase_.load(std::memory_order_acquire) == AbortResetOwnerPhase::Succeeded;
    }

    AbortResetOwnerPhase Phase() const { return phase_.load(std::memory_order_acquire); }

private:
    std::atomic<AbortResetOwnerPhase> phase_{AbortResetOwnerPhase::Idle};
};

enum class StreamQuiescence : uint8_t {
    Idle = 0,
    Active,
    Quiesced,
    Failed,
};

inline constexpr bool CanResetAfterStream(StreamQuiescence state) {
    return state == StreamQuiescence::Idle || state == StreamQuiescence::Quiesced;
}

inline constexpr StreamQuiescence FinishStream(bool proven_quiesced) {
    return proven_quiesced ? StreamQuiescence::Quiesced : StreamQuiescence::Failed;
}

/** 灌行前闸的决策结果；socket 故障由调用方单独处理。 */
enum class StreamSendCancel : uint8_t {
    Allowed = 0,
    Paused,
    Aborted,
};

/**
 * 普通灌行前闸判定：把 stream_mutex_ 内快照与发送前的最新原子读捏成一次决策。
 * 语义是「拒绝集 ≥ 持锁直接检查」：abort 判定改为「状态位置位，或控制纪元
 * 在快照后前进」。pause/abort 提交方在发布暂停/abort 状态的同时（同一把
 * stream_mutex_ 内）递增 stream_control_epoch_，因此快照后发生的提交必然被
 * 快照 epoch 捕获。换纸行分支仍在 stream_mutex_ 内检查 paused，abort 则由循环顶
 * 的排流逻辑兜底。
 */
inline constexpr StreamSendCancel DecideStreamSend(bool snap_abort, bool snap_paused,
                                                   uint32_t snap_epoch, uint32_t latest_epoch) {
    if (snap_abort || latest_epoch != snap_epoch) {
        return StreamSendCancel::Aborted;
    }
    return snap_paused ? StreamSendCancel::Paused : StreamSendCancel::Allowed;
}

/** pause/abort 提交方的纪元递增（允许回绕；ABA 需 2^32 次提交，实际不可达）。 */
inline constexpr uint32_t NextStreamControlEpoch(uint32_t epoch) { return epoch + 1U; }

/** 收分支等 ok 循环里单个等待片的记账方式。 */
enum class RecvWaitTick : uint8_t {
    Accrue = 0,     // 未暂停：计入等 ok 时钟
    FreezePaused,   // 暂停中：冻结 ok 时钟，只累计暂停时长
    PauseTimedOut,  // 暂停累计达上限：提交 abort，走 reset owner 收敛
};

/**
 * 暂停期收分支的等 ok 计时决策。Hold 期 Grbl 主循环阻塞在挂起自旋，在途行
 * 躺在 RX 缓冲不被解析、ok 不会到来——此刻的静默不是失联，等 ok 时钟必须
 * 冻结，否则 30s 运动超时一到任务按「等 ok 超时」死掉，既不发 `~` 也不
 * reset，写字机永久卡 Hold。冻结也不是无限：暂停累计达 max_pause_ms 后
 * 必须走与 WaitWhilePaused 相同的暂停超时收敛（受控 0x18 能把 Hold 一并清掉）。
 */
inline constexpr RecvWaitTick DecideRecvWaitTick(bool paused, uint32_t paused_ms,
                                                 uint32_t max_pause_ms) {
    if (!paused) {
        return RecvWaitTick::Accrue;
    }
    return paused_ms >= max_pause_ms ? RecvWaitTick::PauseTimedOut : RecvWaitTick::FreezePaused;
}

/** 授权探测可重试失败的处置。 */
enum class AuthProbeFailure : uint8_t {
    RetryLater = 0,  // 次数未耗尽：延迟后重新从 `$I` 起探
    FailClosed,      // 耗尽：Failed 终态，等连接重建
};

/**
 * 裸连接授权探测失败的有界重试判定。残留 Hold 时 `$I` 撞 Grbl 的 idleOrAlarm
 * 门回 error:8；一击置 Failed 且唯一清除点是连接重建的话，Hold 期 `?` 有应答、
 * silent-poll 不判死、keepalive 不触发——连接活着就永不 ready。重试给瞬态
 * 非 Idle（Run 将尽、外部上位机解除 Hold）自愈机会；持续 Hold 在耗尽后仍
 * fail closed，绝不代替用户复位（0x18 方案已否决，与换纸保护冲突）。
 */
inline constexpr AuthProbeFailure DecideAuthProbeFailure(int retries_done, int max_retries) {
    return retries_done < max_retries ? AuthProbeFailure::RetryLater : AuthProbeFailure::FailClosed;
}

/** 重试到期判定：now >= due 即到期；RFC 1982 风格半区间比较，容忍 tick 回绕。 */
inline constexpr bool AuthProbeRetryDue(uint32_t now_tick, uint32_t due_tick) {
    return static_cast<uint32_t>(now_tick - due_tick) < (uint32_t{1} << 31);
}

/**
 * R22-PIPE-02：Grbl 处于挂起态（Hold/Door/Sleep）时行命令根本不会被消费。
 * 实测源码面：`protocol_exec_rt_suspend()`（Grbl_Esp32/src/Protocol.cpp:880）
 * 的 `while (sys.suspend.value)` 循环内没有任何 `protocol_poll_client()` 调用，
 * 该函数的 5 个调用点全在 `protocol_main_loop` 侧；行字符只被 `clientCheckTask`
 * 收进 client_buffer 排队，不解析、不应答。而实时字符（`?`/`~`/`!`）走中断读
 * 路径的 `execute_realtime_command()`，挂起期照常应答。
 *
 * 后果：挂起期发出的 `$I` 既不回 `ok` 也不回 `error`——`error:8` 那条 idleOrAlarm
 * 门在挂起期压根到不了。于是 WaitingBuildInfoOk 无声挂死：`?` 有应答 →
 * silent-poll 不判死、keepalive 不触发、错误驱动的有界重试（R10-PIPE-01）也
 * 不被触发，连接活着就永不 ready。
 */
inline constexpr bool GrblSuspendBlocksLines(bool hold, bool door, bool sleep) {
    return hold || door || sleep;
}

/** 探测等应答无声超时的处置。 */
enum class AuthProbeStall : uint8_t {
    KeepWaiting = 0,  // 未到判定拍数：继续等
    ParkSuspended,    // 已超时且对端挂起：不重发（会在 client_buffer 里堆积），等挂起解除
    Reprobe,          // 已超时且对端未挂起：`$I` 确实丢了，按有界重试重探
};

/**
 * 无声超时判定。由 recv 超时拍（EAGAIN 分支，kPollIntervalSec 一拍）驱动，不引
 * 入额外定时器。挂起态一律 ParkSuspended：重发 `$I` 只会在对端 client_buffer 里
 * 排队，挂起解除后一次性全部执行并回出多个 `ok`，与后续探测阶段的应答错配。
 * 亦不代发 `~`（未经用户同意恢复运动）或 `0x18`（毁状态），两者均已否决。
 */
inline constexpr AuthProbeStall DecideAuthProbeStall(int silent_ticks, int stall_limit,
                                                     bool suspend_blocks_lines) {
    if (silent_ticks < stall_limit) {
        return AuthProbeStall::KeepWaiting;
    }
    return suspend_blocks_lines ? AuthProbeStall::ParkSuspended : AuthProbeStall::Reprobe;
}

/**
 * 写字机发现失败分类（2026-08-22）。刷机/掉电后 Grbl `MAX_TLNT_CLIENTS=1`
 * 仍占旧半开连接时，新 TCP 会被立刻踢掉（上游 TelnetServer 拒绝新客户端；
 * FluidNC #189：硬复位客户端不发关闭）。旧实现随后扫 /24 并跳过缓存 IP，
 * 把 ~19s keepalive 放大成分钟级。
 */
enum class DiscoverMiss : uint8_t {
    WaitingIp = 0,     // STA 还没拿到地址
    SlotBusy,          // 缓存 IP 连上后立刻被关（唯一槽位被半开占用）
    CacheUnreachable,  // 缓存 IP TCP 超时/拒绝
    CacheNotGrbl,      // 连上了但 banner 不是写字机
    ScanEmpty,         // 无缓存或扫网未找到
};

inline constexpr int kPipeScanTimeoutMs = 200;     // 盖住首包 modem-sleep（实机 ping 199ms）
inline constexpr int kPipeCachedTimeoutMs = 2000;  // 缓存命中容忍短暂不可达
inline constexpr uint32_t kSlotBusyRetryDelayMs = 1000;
inline constexpr uint32_t kWaitingIpRetryDelayMs = 1000;

/** 扫网时是否跳过缓存 IP：只有验明「不是写字机」才跳过。 */
inline constexpr bool SkipCachedIpDuringScan(DiscoverMiss miss) {
    return miss == DiscoverMiss::CacheNotGrbl;
}

/** 槽位忙或还没 IP 时禁止扫网。 */
inline constexpr bool ShouldScanSubnet(DiscoverMiss miss) {
    return miss != DiscoverMiss::WaitingIp && miss != DiscoverMiss::SlotBusy;
}

/** 扫网可能用短超时漏掉真机时，再用缓存超时打一次。 */
inline constexpr bool RetryCachedIpAfterScan(DiscoverMiss miss, bool found) {
    return !found && miss == DiscoverMiss::CacheUnreachable;
}

inline constexpr uint32_t DiscoverRetryDelayMs(DiscoverMiss miss, uint32_t exponential_backoff_ms) {
    if (miss == DiscoverMiss::SlotBusy) {
        return kSlotBusyRetryDelayMs;
    }
    if (miss == DiscoverMiss::WaitingIp) {
        return kWaitingIpRetryDelayMs;
    }
    return exponential_backoff_ms;
}

inline constexpr bool ShouldAdvanceDiscoverBackoff(DiscoverMiss miss) {
    return miss != DiscoverMiss::SlotBusy && miss != DiscoverMiss::WaitingIp;
}

// 写字机重连退避前段：前 N 次「真失败」（会推进退避的 miss）保持 1s 间隔，
// 之后才指数翻倍至 30s 封顶。断联多由 WiFi 瞬断/对端重启引起，前段密集重试
// 把「断联感知时长」从最坏 30s 压到秒级；真关机场景指数段仍在，不构成风暴。
inline constexpr int kDiscoverFastRetryAttempts = 5;

/** 第 attempt 次（1 起）连续真失败后的下一档退避：前段不涨，过后翻倍封顶。 */
inline constexpr uint32_t NextDiscoverBackoffMs(int attempt, uint32_t current_ms, uint32_t max_ms) {
    if (attempt <= kDiscoverFastRetryAttempts) {
        return current_ms;
    }
    return current_ms * 2 > max_ms ? max_ms : current_ms * 2;
}

/**
 * 挂起解除后的自愈判定：仍在等 `$I` 应答且挂起刚解除 → 排队中的 `$I` 即将被
 * 消费，只需清零无声计数给它一个完整窗口，不得重发（重发即多一个 `ok`）。
 */
inline constexpr bool ShouldRearmStalledProbe(bool waiting_build_info, bool was_suspended,
                                              bool now_suspended) {
    return waiting_build_info && was_suspended && !now_suspended;
}

/**
 * 命令字前缀匹配（词边界）：行以 word 开头且下一字符不是数字才算命中。
 * 裸前缀匹配会把 `M300` 当成 `M30`——换纸行匹配（LooksLikePaperLine）用它
 * 决定 90s 逐行预算与 error:8 重发，误配面按词边界收掉。
 */
inline bool HasGcodeCommandPrefix(const std::string& line, const char* word) {
    const size_t n = std::char_traits<char>::length(word);
    if (line.compare(0, n, word) != 0) {
        return false;
    }
    return line.size() == n || line[n] < '0' || line[n] > '9';
}

/**
 * 换纸遥测三态：Unknown 是 fail-open 默认态——超时/序号未推进/字段缺席都不得
 * 被解释成「缺纸」，否则一次 Telnet 抖动就会误拒有纸的正常出图（比页尾 error:90 更糟）。
 */
enum class PaperPresentState : uint8_t { Unknown = 0, Yes, No };
enum class MotorEnState : uint8_t { Unknown = 0, On, Off };
enum class PanelHoldState : uint8_t { Unknown = 0, On, Off };

inline const char* PaperPresentStateName(PaperPresentState state) {
    switch (state) {
        case PaperPresentState::Yes:
            return "yes";
        case PaperPresentState::No:
            return "no";
        default:
            return "unknown";
    }
}
inline const char* MotorEnStateName(MotorEnState state) {
    switch (state) {
        case MotorEnState::On:
            return "on";
        case MotorEnState::Off:
            return "off";
        default:
            return "unknown";
    }
}
inline const char* PanelHoldStateName(PanelHoldState state) {
    switch (state) {
        case PanelHoldState::On:
            return "on";
        case PanelHoldState::Off:
            return "off";
        default:
            return "unknown";
    }
}

/**
 * 解析 [ESP901] 应答行的 Paper/MotorEn/PanelHold 三字段（2026-08-24 P1-1，对齐协议 §4）。
 * 与 Changing 解析同一守卫：「Paper= 与 Changing= 同现」才认行——防止把其他含
 * `Paper=` 的日志误当遥测；任一字段缺席保持调用方传入的 Unknown，部分应答不整行作废。
 * 返回该行是否为遥测行（守卫通过）。
 */
inline bool ParsePaperStatusFields(const std::string& line, PaperPresentState& paper,
                                   MotorEnState& motor, PanelHoldState& panel) {
    if (line.find("Paper=") == std::string::npos || line.find("Changing=") == std::string::npos) {
        return false;
    }
    if (line.find("Paper=OK") != std::string::npos) {
        paper = PaperPresentState::Yes;
    } else if (line.find("Paper=No") != std::string::npos) {
        paper = PaperPresentState::No;
    }
    if (line.find("MotorEn=On") != std::string::npos) {
        motor = MotorEnState::On;
    } else if (line.find("MotorEn=Off") != std::string::npos) {
        motor = MotorEnState::Off;
    }
    if (line.find("PanelHold=On") != std::string::npos) {
        panel = PanelHoldState::On;
    } else if (line.find("PanelHold=Off") != std::string::npos) {
        panel = PanelHoldState::Off;
    }
    return true;
}


/**
 * Grbl `$` 设置指纹（2026-08-29 实机 COM13 `$$` 只读取证 + protocol §6）。
 * 奎享/WebUI 可改写 NVS 持久化设置，小派须 fail-closed 拒画并上报 mismatch。
 */
struct GrblSettingGolden {
    const char* query_line;    // 发往 Grbl 的行（SendLine 自动补 \\n）
    const char* response_key;  // 应答 `$KEY=VALUE` 中的 KEY
    double expected;
    bool integer;
};

inline constexpr GrblSettingGolden kGrblSettingGoldens[] = {
    {"$1", "1", 255.0, true},
    {"$3", "3", 7.0, true},
    {"$20", "20", 0.0, true},
    {"$21", "21", 0.0, true},
    {"$22", "22", 0.0, true},
    {"$100", "100", 100.0, false},
    {"$101", "101", 100.0, false},
    {"$110", "110", 12000.0, false},
    {"$111", "111", 12000.0, false},
    {"$130", "130", 210.0, false},
    {"$131", "131", 297.0, false},
    {"$132", "132", 200.0, false},
    // 末项查询故意用 Grbl 扩展名括号语法 `[Errors/Verbose]`（§9-G′ 守卫对象的对应查询
    // 形态）；应答仍是 `$Errors/Verbose=Off`，由 ParseGrblSettingLine 的 `$` 前缀路径解析，
    // Off→0 映射见该函数。findings #30 R5：两种语法并存非矛盾，COM14 实机指纹 13 项通过实证。
    {"[Errors/Verbose]", "Errors/Verbose", 0.0, true},
};

inline constexpr size_t kGrblSettingGoldenCount =
    sizeof(kGrblSettingGoldens) / sizeof(kGrblSettingGoldens[0]);

inline bool ParseGrblSettingLine(const std::string& line, std::string& key_out, double& value_out) {
    if (line.empty() || line[0] != '$') {
        return false;
    }
    const size_t eq = line.find('=');
    if (eq <= 1 || eq == std::string::npos) {
        return false;
    }
    key_out = line.substr(1, eq - 1);
    const std::string raw_value = line.substr(eq + 1);
    // FlagSetting 单查走 getStringValue()，应答为 Off/On 而非 compatible 的 0/1。
    if (raw_value == "Off" || raw_value == "off") {
        value_out = 0.0;
        return true;
    }
    if (raw_value == "On" || raw_value == "on") {
        value_out = 1.0;
        return true;
    }
    const char* start = raw_value.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(start, &end);
    if (end == start || (end != nullptr && *end != '\0')) {
        return false;
    }
    if (!std::isfinite(parsed)) {
        return false;
    }
    value_out = parsed;
    return true;
}

inline bool GrblSettingValueMatches(double actual, double expected, bool integer) {
    if (integer) {
        const double rounded = std::round(actual);
        return std::abs(actual - rounded) < 1e-6 &&
               static_cast<long long>(rounded) == static_cast<long long>(expected);
    }
    return std::abs(actual - expected) <= 0.001;
}

struct GrblSettingCheckResult {
    bool ok = false;
    std::string key;
    double expected = 0.0;
    double actual = 0.0;
};

inline GrblSettingCheckResult CheckGrblSettingAgainstGolden(size_t index, const std::string& key,
                                                            double actual) {
    GrblSettingCheckResult result;
    result.actual = actual;
    if (index >= kGrblSettingGoldenCount) {
        result.key = key;
        return result;
    }
    const GrblSettingGolden& golden = kGrblSettingGoldens[index];
    result.key = golden.response_key;
    result.expected = golden.expected;
    if (key != golden.response_key) {
        return result;
    }
    result.ok = GrblSettingValueMatches(actual, golden.expected, golden.integer);
    return result;
}

/**
 * 预览重入幂等（2026-08-24 P1-3 配套）：服务端链式调用成功在预览后，云端 LLM 的
 * 第二步会以同 url/preview_url 重入 StartDraw；busy 已占但任务停在
 * awaiting_confirmation 且参数完全相同时，应回 previewing（等价「已在等确认」），
 * 而不是把「写字机正忙」失败文案念给眼前有预览的用户。仅在参数不同或未在
 * 等确认（出图中）时才判 busy。
 */
inline constexpr bool IsDuplicatePreviewReentry(bool awaiting_confirmation, bool same_urls) {
    return awaiting_confirmation && same_urls;
}

class AbortResetToken {
public:
    /** 调用方必须用同一把发送锁串行化 Arm/Consume/Cancel。 */
    bool Arm(uint32_t connection_generation, uint32_t current_banner_generation,
             uint32_t current_receive_epoch) {
        const uint32_t expected_banner = NextGeneration(current_banner_generation);
        State state = state_.load(std::memory_order_acquire);
        while (true) {
            if (state == State::Arming || state == State::Pending) {
                return false;
            }
            // 已取消的 reset 结果仍可能迟到；同一 session/同一代不得重新布防，
            // 否则旧 banner 会兑现新 token（ABA）。换 session 或 banner 前进后才可重试。
            if (state == State::Cancelled &&
                connection_generation_.load(std::memory_order_relaxed) == connection_generation &&
                expected_banner_generation_.load(std::memory_order_relaxed) == expected_banner) {
                return false;
            }
            if (state_.compare_exchange_weak(state, State::Arming, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        connection_generation_.store(connection_generation, std::memory_order_relaxed);
        expected_banner_generation_.store(expected_banner, std::memory_order_relaxed);
        expected_receive_epoch_.store(current_receive_epoch, std::memory_order_relaxed);
        State expected = State::Arming;
        return state_.compare_exchange_strong(expected, State::Pending, std::memory_order_release,
                                              std::memory_order_acquire);
    }

    bool Consume(uint32_t connection_generation, uint32_t banner_generation,
                 uint32_t receive_epoch) {
        State state = state_.load(std::memory_order_acquire);
        while (state == State::Pending || state == State::Cancelled) {
            // 早到/旧 session banner 不得破坏当前 token；只消费精确绑定的一代。
            if (connection_generation_.load(std::memory_order_relaxed) != connection_generation ||
                expected_banner_generation_.load(std::memory_order_relaxed) != banner_generation ||
                !IsAfter(receive_epoch, expected_receive_epoch_.load(std::memory_order_relaxed))) {
                return false;
            }
            const bool redeem = state == State::Pending;
            if (state_.compare_exchange_weak(state, State::Empty, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return redeem;
            }
        }
        return false;
    }

    void Cancel() {
        State state = state_.load(std::memory_order_acquire);
        while (state == State::Arming || state == State::Pending) {
            if (state_.compare_exchange_weak(state, State::Cancelled, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return;
            }
        }
    }

    bool Pending() const { return state_.load(std::memory_order_acquire) == State::Pending; }

    static constexpr uint32_t NextGeneration(uint32_t generation) { return generation + 1U; }

    /** RFC 1982 风格半区间比较，支持 UINT32_MAX→0。 */
    static constexpr bool IsAfter(uint32_t candidate, uint32_t baseline) {
        const uint32_t distance = candidate - baseline;
        return distance != 0U && distance < (uint32_t{1} << 31);
    }

private:
    enum class State : uint8_t {
        Empty = 0,
        Arming,
        Pending,
        Cancelled,
    };

    std::atomic<uint32_t> connection_generation_{0};
    std::atomic<uint32_t> expected_banner_generation_{0};
    std::atomic<uint32_t> expected_receive_epoch_{0};
    std::atomic<State> state_{State::Empty};
};

}  // namespace hutuji

#endif  // HUTUJI_RECOVERY_CORE_H
