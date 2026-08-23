#ifndef PLOTTER_PROVISION_CORE_H
#define PLOTTER_PROVISION_CORE_H

/**
 * 设备端零接触配网（写字机）的纯逻辑核心：命令序列、URL 编码、应答分类、
 * 凭据校验、失败原因与退避策略。header-only，host 测试直接编译本头
 * （scripts/tests/test_plotter_provision_core.py），不依赖任何 ESP-IDF 头。
 *
 * 写字机侧事实源（Grbl_Esp32 仓，本特性对它零改动）：
 * - 出厂 AP：SSID GRBL_ESP / 密码 12345678 / 192.168.0.1（WifiConfig.h:54-58）。
 * - 命令面：`GET /command?plain=[ESPxxx]`，无输出时 200 + 体 "ok"；出错且无输出
 *   时 500 + 体 "Error: <文本>"；出错但有流式输出时仍可能 200（WebServer.cpp
 *   _handle_web_command :442-487），故分类必须看体，不能只看状态码。
 * - ESP444 RESTART 的 setSystemMode 立即 ESP.restart()（WebSettings.cpp:412-421），
 *   应答可能永远发不出来，传输失败按「已发出」处理，靠重启后的既有发现验证。
 * - 设置校验边界：SSID 1..32 且逐字节可打印（isSSIDValid → Arduino isPrintable，
 *   C locale 下即 0x20..0x7E；中文 SSID 的 UTF-8 字节 >=0x80 会被拒）；
 *   密码 0（开放网络）或 8..64（WifiConfig.h:69-74 + isPasswordValid :160-171）。
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace hutuji {
namespace provision {

// 写字机出厂 AP 常量（Grbl_Esp32 `WifiConfig.h:54-58` 字面量）。
inline constexpr char kPlotterApSsid[] = "GRBL_ESP";
inline constexpr char kPlotterApPassword[] = "12345678";
inline constexpr char kPlotterApHost[] = "192.168.0.1";

// 时间参数。单次跳配时间盒必须远在 waveshare 断连看门狗 120s 以内
// （esp32-s3-touch-lcd-3.5.cc StartWifiLostWatchdog），否则回切途中被踹进配网模式；
// 该不变量有 host 测试钉死。
inline constexpr uint32_t kPatrolDelayMs = 3000;          // Connected 后等既有发现再巡检
inline constexpr uint32_t kPipeGraceMs = 15000;           // 巡检期等管道发现的轮询上限
inline constexpr uint32_t kJumpConnectTimeoutMs = 10000;  // 跳配连出厂 AP 的 GOT_IP 超时
inline constexpr uint32_t kHttpTimeoutMs = 5000;          // 单条命令的 HTTP 超时
inline constexpr uint32_t kRestoreTimeoutMs = 30000;      // 回切户网等 Connected 的上限
inline constexpr uint32_t kVerifyTimeoutMs = 60000;       // 写字机重启后等管道接管的上限
inline constexpr int kMaxAutoAttempts = 3;                // 每次 Connected 的自动尝试上限

/** 第 attempt 次（1 起）自动失败后的退避；递增、分钟级，防重试风暴。 */
inline constexpr uint32_t AutoRetryDelayMs(int attempt) { return attempt <= 1 ? 60000u : 180000u; }

/** 户网凭据校验结果；镜像写字机 WebSettings 的 StringSetting 边界，先检先报。 */
enum class CredentialError {
    None = 0,
    SsidTooShort,
    SsidTooLong,
    SsidNotPrintable,
    PasswordLengthInvalid,
};

inline CredentialError ValidateHomeCredentials(const std::string& ssid,
                                               const std::string& password) {
    if (ssid.empty()) {
        return CredentialError::SsidTooShort;
    }
    if (ssid.size() > 32) {
        return CredentialError::SsidTooLong;
    }
    for (const unsigned char ch : ssid) {
        if (ch < 0x20 || ch > 0x7E) {
            return CredentialError::SsidNotPrintable;
        }
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 64)) {
        return CredentialError::PasswordLengthInvalid;
    }
    return CredentialError::None;
}

/**
 * query 值编码：unreserved（A-Za-z0-9-_.~）原样，其余逐字节 %XX 大写十六进制。
 * 空格必须 %20 而非 '+'：Arduino WebServer 的 arg() 只解 %XX 三元组。
 */
inline std::string UrlEncode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
                                ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", ch);
            out.append(buf, 3);
        }
    }
    return out;
}

/**
 * 配置命令序列：与量产串口脚本 scripts/grbl_wifi_setup.py 同集同序——三条 NVS
 * 写入（重启生效，ESP110 不会当场断 AP）+ 最后的 RESTART。任一写失败即中止，
 * 不发后续命令、不 RESTART（不把「SSID 已改密码没改」的半配置状态重启上线）。
 */
inline std::array<std::string, 4> BuildCommandSequence(const std::string& ssid,
                                                       const std::string& password) {
    return {"[ESP100]" + ssid, "[ESP101]" + password, "[ESP110]STA", "[ESP444]RESTART"};
}

/** 命令 URL：固定打写字机出厂 AP 的 WebUI 命令面。 */
inline std::string BuildCommandUrl(const std::string& command) {
    return std::string("http://") + kPlotterApHost + "/command?plain=" + UrlEncode(command);
}

/** 单条命令的应答分类。 */
enum class EspCmdResult {
    Ok = 0,
    Rejected,        // 对端明确拒绝（error 行 / Error: 文本 / HTTP>=400）
    TransportError,  // 未拿到响应（连接失败/对端已重启）
};

/**
 * 应答分类，口径同 scripts/grbl_wifi_setup.py classify_response：
 * error 行优先于一切 ok 样内容；成功只认独立 `ok` 行或 `:ok` 尾缀——SSID/密码
 * 本身可能含 ok 子串，子串判定会把失败读成成功并继续发 RESTART。HTTP 侧另认
 * 大写 `Error: <文本>` 形态（WebServer.cpp:475）与 >=400 状态码。无 ok 行的
 * 未知体按 Rejected 处理：宁可中止也不带病重启。
 */
inline EspCmdResult ClassifyEspResponse(int http_status, const std::string& body) {
    if (http_status < 200) {
        return EspCmdResult::TransportError;
    }
    if (http_status >= 400) {
        return EspCmdResult::Rejected;
    }
    // 逐行判定：error 先行（全帧扫描），ok 后行。
    bool has_ok = false;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t head = line.find_first_not_of(' ');
        if (head == std::string::npos) {
            if (end == body.size()) {
                break;
            }
            continue;
        }
        std::string low;
        low.reserve(line.size() - head);
        for (size_t i = head; i < line.size(); ++i) {
            const char c = line[i];
            low.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        }
        if (low.rfind("error:", 0) == 0 || low.find(":error:") != std::string::npos) {
            return EspCmdResult::Rejected;
        }
        if (low == "ok" || (low.size() > 3 && low.compare(low.size() - 3, 3, ":ok") == 0)) {
            has_ok = true;
        }
        if (end == body.size()) {
            break;
        }
    }
    return has_ok ? EspCmdResult::Ok : EspCmdResult::Rejected;
}

/** ESP444 的应答可能因对端立即重启而永远发不出：传输失败按「已发出」处理。 */
inline bool IsRestartOutcomeAcceptable(EspCmdResult result) {
    return result == EspCmdResult::Ok || result == EspCmdResult::TransportError;
}

/**
 * 空口嗅探帧判定：payload 是 beacon(0x80) 或 probe response(0x50) 且其 SSID IE
 * 与 target 逐字节相等（长度也相等，防 "GRBL_ESP2" 这类前缀误中）。
 * 802.11 管理帧布局：MAC 头 24B（frame control 2B 起，toDS/fromDS 必须为 0——
 * 置位则头为 30B，布局不同，直接不认）+ fixed params 12B（timestamp 8 +
 * interval 2 + capability 2），IE 链自 offset 36 起，SSID 为 tag 0、长度 0..32。
 * 存在理由：2026-08-23 HIL 实测 esp-wifi-connect 的 WifiStation 会处理任何一次
 * 扫描完成并无条件 esp_wifi_connect 户网，连接动作清掉扫描结果；且连接态过滤
 * 扫描的结果集本身也可能漏收。混杂模式嗅探 beacon/probe response 不经过扫描
 * 结果集，收到一帧即为在场铁证。混杂回调跑在 wifi 任务上下文，只调本函数。
 */
inline bool ProbeFrameMatchesSsid(const uint8_t* payload, size_t len, const char* target) {
    if (payload == nullptr || target == nullptr || len < 38) {
        return false;
    }
    const uint8_t fc0 = payload[0];
    if (fc0 != 0x80 && fc0 != 0x50) {  // beacon / probe response（type mgmt）
        return false;
    }
    if ((payload[1] & 0x03) != 0) {  // toDS/fromDS 置位的帧头布局不同，不认
        return false;
    }
    const size_t target_len = std::strlen(target);
    if (target_len > 32) {
        return false;
    }
    size_t pos = 36;  // IE 链起点
    while (pos + 2 <= len) {
        const uint8_t tag = payload[pos];
        const uint8_t ie_len = payload[pos + 1];
        if (pos + 2 + ie_len > len) {
            return false;  // IE 声明长度越界 = 帧截断/畸形，停
        }
        if (tag == 0) {
            return ie_len == target_len &&
                   std::memcmp(payload + pos + 2, target, ie_len) == 0;
        }
        pos += 2 + ie_len;
    }
    return false;
}

/** 配网失败原因（用户话术分类，技术细节走 ESP 日志）。 */
enum class ProvisionFailure {
    ApNotInRange = 0,       // 扫不到写字机出厂热点（未被重置/距离太远）
    ApAuthFailed,           // 出厂 AP 连不上（密码被改/射频瞬态）
    CommandRejected,        // 配置命令被写字机拒绝
    NotOnlineAfterRestart,  // 重启后既有发现窗口内没接管到写字机
    HomeWifiRestoreFailed,  // 回切户网未在时限内恢复
    InvalidCredentials,     // 户网凭据过不了写字机侧校验（如中文 SSID）
};

/** 用户面中文话术；技术诊断串不进屏（与 DescribeTransferFailure 同约定）。 */
inline const char* DescribeFailure(ProvisionFailure reason) {
    switch (reason) {
        case ProvisionFailure::ApNotInRange:
            return "没找到待配置的写字机：请确认写字机已上电并处于待配网状态";
        case ProvisionFailure::ApAuthFailed:
            return "连不上写字机的配网热点，请把写字机放近一点再试";
        case ProvisionFailure::CommandRejected:
            return "写字机拒绝了配置：请在写字机屏幕上改用网页配网";
        case ProvisionFailure::NotOnlineAfterRestart:
            return "写字机重启后没连上家里网络：请核对 WiFi 名称和密码";
        case ProvisionFailure::HomeWifiRestoreFailed:
            return "家里的网络恢复失败，请稍等或重启设备";
        case ProvisionFailure::InvalidCredentials:
            return "家里 WiFi 名称含写字机不支持的字符：请在写字机屏幕上改用网页配网";
    }
    return "";
}

/** 自动重试只救瞬态；确定性失败直接转人工话术，不重试风暴。 */
inline bool IsTransient(ProvisionFailure reason) {
    return reason == ProvisionFailure::ApAuthFailed ||
           reason == ProvisionFailure::NotOnlineAfterRestart ||
           reason == ProvisionFailure::HomeWifiRestoreFailed;
}

}  // namespace provision
}  // namespace hutuji

#endif  // PLOTTER_PROVISION_CORE_H
