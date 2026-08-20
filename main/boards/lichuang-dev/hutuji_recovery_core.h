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
 * 点动的唯一防线；限幅与云端 protocol §5 同源（X≤190 / Y≤277mm），改云端必须
 * 同步这里。任一输入非有限数一律 fail-closed 为 kStalePosition——`$10` 可持久化
 * 成 WPos、网络/固件异常可产生 NaN/Inf，都不得冒充「已知坐标」放行运动。
 */
inline constexpr float kJogEnvelopeMaxXMm = 190.0f;
inline constexpr float kJogEnvelopeMaxYMm = 277.0f;
/** 1mm 固定步进，逐字对齐奎享实测 `$J=G21G91X1.0Y0.0Z0.0F8000.0`。 */
inline constexpr float kJogStepMm = 1.0f;

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
        url.compare(path_end - expected_suffix.size(), expected_suffix.size(), expected_suffix) != 0) {
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
