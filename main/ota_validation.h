#pragma once
#include <array>
#include <cstdint>
#include <string>

// 严格版本和摘要校验同时用于工具入口与下载层，避免入口与镜像语义漂移。
inline bool ParseHutujiVersion(const std::string& value, std::array<uint32_t, 3>& out) {
    if (value.compare(0, 7, "hutuji.") != 0)
        return false;
    size_t pos = 7;
    for (size_t i = 0; i < out.size(); ++i) {
        const size_t start = pos;
        uint32_t number = 0;
        while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') {
            if (number > 10000000)
                return false;
            number = number * 10 + value[pos++] - '0';
        }
        if (pos == start || (pos - start > 1 && value[start] == '0'))
            return false;
        out[i] = number;
        if (i != 2 && (pos == value.size() || value[pos++] != '.'))
            return false;
    }
    return pos == value.size();
}
inline bool IsStrictlyNewerHutujiVersion(const std::string& current, const std::string& target) {
    std::array<uint32_t, 3> a{}, b{};
    return ParseHutujiVersion(current, a) && ParseHutujiVersion(target, b) && b > a;
}
inline bool ValidFirmwareSha256(const std::string& sha) {
    if (sha.size() != 64)
        return false;
    for (char c : sha)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}
