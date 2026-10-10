#ifndef HUTUJI_PAPER_CORE_H
#define HUTUJI_PAPER_CORE_H

#include <cmath>
#include <cstdint>
#include <string>

namespace hutuji {

// 与云端 PaperConfig 同源单位：十分之一毫米。2000mm 是输入预算，不是实测行程。
struct PaperConfig {
    std::string paper = "A4";
    int width = 2100;
    int height = 2970;
    bool landscape = false;
    int margin = 100;
    bool swap_xy = true;
    int max_x = 2770;
    int max_y = 1900;

    int PageWidth() const { return (landscape ? height : width) - 2 * margin; }
    int PageHeight() const { return (landscape ? width : height) - 2 * margin; }
};

inline PaperConfig DefaultPaperConfig(bool nopaper) {
    PaperConfig value;
    if (nopaper) {
        value.swap_xy = false;
        value.max_x = 2050;
        value.max_y = 2900;
    }
    return value;
}

inline bool ValidatePaperConfig(const PaperConfig& value) {
    if (value.width < 500 || value.width > 20000 || value.height < 500 || value.height > 20000 ||
        value.margin < 0 || value.margin > 500 || value.max_x < 100 || value.max_x > 20000 ||
        value.max_y < 100 || value.max_y > 20000) {
        return false;
    }
    if (value.paper == "A4") {
        if (value.width != 2100 || value.height != 2970)
            return false;
    } else if (value.paper == "A3") {
        if (value.width != 2970 || value.height != 4200)
            return false;
    } else if (value.paper == "A2") {
        if (value.width != 4200 || value.height != 5940)
            return false;
    } else if (value.paper != "custom") {
        return false;
    }
    const int width = value.PageWidth();
    const int height = value.PageHeight();
    return width >= 200 && height >= 200 && (value.swap_xy ? height : width) <= value.max_x &&
           (value.swap_xy ? width : height) <= value.max_y;
}

inline bool PaperMillimetersToTenths(double value, int& result) {
    if (!std::isfinite(value) || value < 0 || value > 2000)
        return false;
    const double scaled = value * 10;
    const double rounded = std::round(scaled);
    if (std::abs(scaled - rounded) > 1e-7)
        return false;
    result = static_cast<int>(rounded);
    return true;
}

inline std::string PaperMarker(const PaperConfig& value) {
    return "p1:" + value.paper + ":" + std::to_string(value.width) + ":" +
           std::to_string(value.height) + ":" + (value.landscape ? "1" : "0") + ":" +
           std::to_string(value.margin) + ":" + (value.swap_xy ? "1" : "0") + ":" +
           std::to_string(value.max_x) + ":" + std::to_string(value.max_y);
}

inline bool operator==(const PaperConfig& left, const PaperConfig& right) {
    return PaperMarker(left) == PaperMarker(right);
}

inline bool ParsePaperMarker(const std::string& text, PaperConfig& output) {
    if (text.size() > 96 || text.rfind("p1:", 0) != 0)
        return false;
    std::string parts[8];
    size_t begin = 3;
    for (int i = 0; i < 8; ++i) {
        const size_t end = text.find(':', begin);
        if ((i < 7 && end == std::string::npos) || (i == 7 && end != std::string::npos))
            return false;
        parts[i] = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (parts[i].empty())
            return false;
        begin = end == std::string::npos ? text.size() : end + 1;
    }
    int numbers[7]{};
    for (int i = 0; i < 7; ++i) {
        const auto& token = parts[i + 1];
        if (token.size() > 5)
            return false;
        for (char c : token) {
            if (c < '0' || c > '9')
                return false;
            numbers[i] = numbers[i] * 10 + c - '0';
        }
    }
    if (numbers[2] > 1 || numbers[4] > 1)
        return false;
    PaperConfig value{parts[0],   numbers[0],      numbers[1], numbers[2] == 1,
                      numbers[3], numbers[4] == 1, numbers[5], numbers[6]};
    // 只接收规范形式：重复分隔符、前导零、尾部数据均不能成为第二种身份。
    if (!ValidatePaperConfig(value) || PaperMarker(value) != text)
        return false;
    output = value;
    return true;
}

inline bool PaperRangeIncreased(const PaperConfig& before, const PaperConfig& after) {
    return after.max_x > before.max_x || after.max_y > before.max_y;
}

inline bool PaperHeaderMatches(const std::string& header, const PaperConfig& current,
                               bool nopaper) {
    if (!ValidatePaperConfig(current))
        return false;
    return header.empty() ? current == DefaultPaperConfig(nopaper) : header == PaperMarker(current);
}

inline bool PaperChangeAllowed(const std::string& state, bool busy, bool preview_worker,
                               bool reset_worker, bool paper_active) {
    return !busy && !preview_worker && !reset_worker && !paper_active &&
           (state == "idle" || state == "done" || state == "error" || state == "aborted");
}

}  // namespace hutuji

#endif
