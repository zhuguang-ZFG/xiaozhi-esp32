#pragma once
#include <cstddef>
#include <cstdint>

// SSID按UTF-8字节限32；拒绝损坏编码、控制字符、代理区与超范围码点。
inline bool ValidUtf8Ssid(const char* text, size_t size) {
    if (!text || size == 0 || size > 32)
        return false;
    for (size_t i = 0; i < size;) {
        uint32_t c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f)
                return false;
            continue;
        }
        unsigned need;
        uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            c &= 0x1f;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            c &= 0x0f;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            c &= 7;
            minimum = 0x10000;
        } else
            return false;
        if (i + need > size)
            return false;
        while (need--) {
            unsigned b = static_cast<unsigned char>(text[i++]);
            if ((b & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (b & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            (c >= 0x80 && c <= 0x9f) || c == 0x2028 || c == 0x2029 ||
            (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069))
            return false;
    }
    return true;
}
