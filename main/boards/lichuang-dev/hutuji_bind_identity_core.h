#ifndef HUTUJI_BIND_IDENTITY_CORE_H
#define HUTUJI_BIND_IDENTITY_CORE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hutuji {

constexpr int kBindIdentityVersion = 2;

struct BindIdentitySession {
    std::string device_id;
    std::string public_key;
    std::string nonce;
    std::string bind_code;
};

enum class BindRunMode { Stopped, Headless, Manual };

// 调用方持锁；代次使慢 HTTP 回包和延迟 UI 回调不能覆盖新二维码。
struct BindIdentityRun {
    uint64_t generation = 0;
    BindRunMode mode = BindRunMode::Stopped;
    BindIdentitySession session;

    void Request(const BindIdentitySession& next, BindRunMode next_mode) {
        ++generation;
        session = next;
        mode = next_mode;
    }
    void Stop() {
        ++generation;
        mode = BindRunMode::Stopped;
    }
    bool Current(uint64_t expected) const {
        return expected == generation && mode != BindRunMode::Stopped;
    }
    bool Complete(uint64_t expected) {
        if (!Current(expected))
            return false;
        mode = BindRunMode::Stopped;
        return true;
    }
};

inline bool BindCanonicalBase64Url(const std::string& value, size_t bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (value.size() != (bytes * 8 + 5) / 6 || value.empty())
        return false;
    for (char ch : value) {
        if (ch == 0 || std::strchr(alphabet, ch) == nullptr)
            return false;
    }
    const unsigned unused = static_cast<unsigned>((6 - bytes * 8 % 6) % 6);
    return (static_cast<unsigned>(std::strchr(alphabet, value.back()) - alphabet) &
            ((1U << unused) - 1)) == 0;
}

inline std::string BindBase64Url(const uint8_t* data, size_t size) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve((size * 8 + 5) / 6);
    uint32_t bits = 0;
    int count = 0;
    for (size_t i = 0; i < size; ++i) {
        bits = (bits << 8) | data[i];
        count += 8;
        while (count >= 6) {
            count -= 6;
            result += alphabet[(bits >> count) & 63];
        }
    }
    if (count != 0) {
        result += alphabet[(bits << (6 - count)) & 63];
    }
    return result;
}

inline std::string BindHex(const uint8_t* data, size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += hex[data[i] >> 4];
        result += hex[data[i] & 15];
    }
    return result;
}

inline std::string BindIdentityQuery(const BindIdentitySession& session) {
    // 这四项只含十六进制/base64url，拼接时不会产生新 query 字段。
    return "&v=2&d=" + session.device_id + "&k=" + session.public_key + "&n=" + session.nonce;
}

inline std::string BindIdentityMessage(const BindIdentitySession& session,
                                       const std::string& challenge, const std::string& mac,
                                       const std::string& sku, const std::string& token_sha256) {
    return "hutuji-bind-v2|" + session.device_id + "|" + session.nonce + "|" + challenge + "|" +
           session.bind_code + "|" + mac + "|" + sku + "|" + token_sha256;
}

}  // namespace hutuji

#endif
