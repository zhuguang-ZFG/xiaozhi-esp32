#ifndef HUTUJI_MUSIC_CORE_H
#define HUTUJI_MUSIC_CORE_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "hutuji_recovery_core.h"  // ParseIpv4 / IsDecimalPort 单一事实源

/**
 * 唱歌功能的纯逻辑核：URL 白名单与播放状态机。header-only，host 可测；
 * 线程/网络/音频一律不进来（与 hutuji_recovery_core.h 同约定）。
 */
namespace hutuji::music {

/** 单曲下载上限：32kbps Opus 下约 17 分钟，远超儿歌长度；PSRAM 全量缓冲。 */
inline constexpr size_t kMaxSongBytes = 4 * 1024 * 1024;
/** 解封装投喂切片：32KiB 一片，片间检查停止标志与设备状态。 */
inline constexpr size_t kDemuxChunkBytes = 32 * 1024;
/** MCP 参数上限：歌名只用于屏显与日志，过长截断在 core 层钉死。 */
inline constexpr size_t kMaxTitleChars = 64;

/**
 * 校验 hutuji.sing 的歌曲 URL。与 IsValidDrawCapabilityUrl 同口径：
 * 生产域只允许 HTTPS；HTTP 仅留给 RFC1918 联调主机；authority 禁
 * userinfo/IPv6/畸形端口。差异仅在路径前缀 `/songs/` 与后缀 `.ogg`。
 */
inline bool IsValidSongUrl(const std::string& url) {
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
        url.compare(slash, 7, "/songs/") != 0 || url.find('#', slash) != std::string::npos) {
        return false;
    }
    const size_t query = url.find('?', slash);
    const size_t path_end = query == std::string::npos ? url.size() : query;
    constexpr const char* kSuffix = ".ogg";
    if (path_end < 4 || url.compare(path_end - 4, 4, kSuffix) != 0) {
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

/** 播放状态机：下载与播放是同一条任务内的两个阶段，外部只能停。 */
enum class MusicState : uint8_t {
    kIdle = 0,
    kDownloading = 1,
    kPlaying = 2,
    kStopping = 3,
};

/**
 * 合法转移表。kStopping 只能回 kIdle（任务退出时）；任何活跃态都可进
 * kStopping（切歌/用户喊停/设备离开 idle）。kIdle 只能进 kDownloading，
 * 不允许「不下载直接播」（没有缓存复用，防止播到半首残曲）。
 */
inline bool CanTransition(MusicState from, MusicState to) {
    switch (from) {
        case MusicState::kIdle:
            return to == MusicState::kDownloading;
        case MusicState::kDownloading:
            return to == MusicState::kPlaying || to == MusicState::kStopping ||
                   to == MusicState::kIdle;  // 下载失败直接退场
        case MusicState::kPlaying:
            return to == MusicState::kStopping || to == MusicState::kIdle;  // 自然唱完
        case MusicState::kStopping:
            return to == MusicState::kIdle;
    }
    return false;
}

}  // namespace hutuji::music

#endif  // HUTUJI_MUSIC_CORE_H
