#pragma once

#include <esp_mac.h>
#include <nvs.h>
#include <array>
#include <cstdint>
#include <string>

namespace hutuji {
// 工厂配对只识别机器，不是用户账号授权；配置损坏不能退回任意Grbl。
struct FactoryIdentity {
    bool present = false;
    bool valid = false;
    std::string sn, grbl, ap;
    std::array<uint8_t, 6> bssid{};
};

inline bool ParseFactoryMac(const std::string& text, std::array<uint8_t, 6>& bytes) {
    if (text.size() != 17)
        return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    bool nonzero = false;
    for (size_t i = 0; i < 6; ++i) {
        int high = digit(text[i * 3]), low = digit(text[i * 3 + 1]);
        if (high < 0 || low < 0 || (i < 5 && text[i * 3 + 2] != ':'))
            return false;
        bytes[i] = static_cast<uint8_t>(high * 16 + low);
        nonzero = nonzero || bytes[i] != 0;
    }
    return nonzero && (bytes[0] & 1) == 0;
}

inline FactoryIdentity LoadFactoryIdentity() {
    FactoryIdentity value;
    nvs_handle_t handle;
    const esp_err_t error = nvs_open("hutuji_factory", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND)
        return value;
    value.present = true;
    if (error != ESP_OK)
        return value;
    auto read = [&](const char* key, std::string& target) {
        char data[64]{};
        size_t length = sizeof(data);
        if (nvs_get_str(handle, key, data, &length) != ESP_OK)
            return false;
        target = data;
        return true;
    };
    uint8_t schema = 0;
    std::string s3;
    bool ok = nvs_get_u8(handle, "schema", &schema) == ESP_OK && schema == 1 &&
              read("sn", value.sn) && read("s3", s3) && read("grbl", value.grbl) &&
              read("ap", value.ap);
    nvs_close(handle);
    std::array<uint8_t, 6> self{}, peer{};
    uint8_t actual[6]{};
    ok = ok && !value.sn.empty() && value.sn.size() <= 48 && ParseFactoryMac(s3, self) &&
         ParseFactoryMac(value.grbl, peer) && ParseFactoryMac(value.ap, value.bssid);
    for (char c : value.sn)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '_' || c == '-'))
            ok = false;
    if (esp_read_mac(actual, ESP_MAC_WIFI_STA) != ESP_OK)
        ok = false;
    for (size_t i = 0; i < 6; ++i)
        if (self[i] != actual[i])
            ok = false;
    if (self == peer || self == value.bssid || peer == value.bssid)
        ok = false;
    value.valid = ok;
    return value;
}

inline bool FactoryPeerMatches(const FactoryIdentity& identity, const std::string& reply) {
    if (!identity.present)
        return true;
    if (!identity.valid)
        return false;
    // SSID允许包含MAC=文本；只接受完整STA报告末尾的真实MAC字段。
    size_t pos = 0;
    while (pos < reply.size()) {
        const auto end = reply.find('\n', pos);
        if (end == std::string::npos)
            return false;
        std::string line = reply.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("[MSG:Mode=STA:SSID=", 0) != 0 || line.empty() || line.back() != ']')
            continue;
        const auto field = line.rfind(":MAC=");
        if (field == std::string::npos || field + 5 + 17 + 1 != line.size())
            continue;
        std::string value = line.substr(field + 5, 17);
        for (char& c : value)
            if (c == '-')
                c = ':';
        std::array<uint8_t, 6> got{}, expected{};
        return ParseFactoryMac(value, got) && ParseFactoryMac(identity.grbl, expected) &&
               got == expected;
    }
    return false;
}
}  // namespace hutuji
