#ifndef HUTUJI_MEMORY_CORE_H
#define HUTUJI_MEMORY_CORE_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

/**
 * 设备侧记忆纯逻辑核：键值清洗、LRU、回忆/删除与 JSON 编解码。
 * header-only，host 可测；NVS/MCP 不进本文件。
 *
 * 同智能体多设备靠「每机一份存储」隔离；条数 80（闪存友好，异于云端 200）。
 */
namespace hutuji::memory {

inline constexpr size_t kMaxEntries = 80;
inline constexpr size_t kKeyMaxChars = 50;
inline constexpr size_t kValueMaxChars = 500;
inline constexpr size_t kRecallAllLimit = 40;
inline constexpr size_t kRecallAllMaxChars = 4000;
/** 单文件 NVS 字符串软上限；超则逐出最旧直至可写。 */
inline constexpr size_t kJsonSoftMaxBytes = 3500;

struct Store {
    /** front = 最久未触碰，back = 最新。 */
    std::vector<std::pair<std::string, std::string>> entries;
};


// 严格解码，拒绝过长编码、代理项和截断字节；字数按 Unicode 码点计算。
inline bool NextCodepoint(const std::string& text, size_t& i, unsigned& cp) {
    if (i >= text.size()) return false;
    const auto lead = static_cast<unsigned char>(text[i++]);
    size_t extra = 0;
    unsigned minimum = 0;
    if (lead < 0x80) { cp = lead; return true; }
    if (lead >= 0xc2 && lead <= 0xdf) { cp = lead & 0x1f; extra = 1; minimum = 0x80; }
    else if (lead >= 0xe0 && lead <= 0xef) { cp = lead & 0x0f; extra = 2; minimum = 0x800; }
    else if (lead >= 0xf0 && lead <= 0xf4) { cp = lead & 7; extra = 3; minimum = 0x10000; }
    else return false;
    while (extra--) {
        if (i >= text.size()) return false;
        const auto c = static_cast<unsigned char>(text[i++]);
        if ((c & 0xc0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3f);
    }
    return cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}

inline size_t CodepointCount(const std::string& text) {
    size_t i = 0, count = 0;
    unsigned cp = 0;
    while (NextCodepoint(text, i, cp)) ++count;
    return count;
}

inline void StripControlAndClamp(std::string& text, size_t limit) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = i;
        unsigned cp = 0;
        // 拒绝整次非法输入，避免不同坏字节被清洗成同一个主题。
        if (!NextCodepoint(text, i, cp)) { text.clear(); return; }
        if (cp < 0x20u || cp == 0x7fu) continue;
        out.append(text, start, i - start);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    const size_t start = out.find_first_not_of(' ');
    out.erase(0, start == std::string::npos ? out.size() : start);
    i = 0;
    unsigned cp = 0;
    for (size_t count = 0; count < limit && i < out.size(); ++count) NextCodepoint(out, i, cp);
    out.resize(i);
    text.swap(out);
}

inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20u) {
                    break;
                }
                out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

inline std::string ToJsonObject(const Store& store) {
    std::string out = "{";
    bool first = true;
    for (const auto& kv : store.entries) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += '"';
        out += JsonEscape(kv.first);
        out += "\":\"";
        out += JsonEscape(kv.second);
        out += '"';
    }
    out += '}';
    return out;
}


inline bool ParseHex4(const std::string& raw, size_t& i, unsigned& value) {
    value = 0;
    for (int n = 0; n < 4; ++n) {
        if (i == raw.size()) return false;
        const char h = raw[i++];
        unsigned digit;
        if (h >= '0' && h <= '9') digit = h - '0';
        else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

inline void AppendCodepoint(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else {
        if (cp >= 0x10000) out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        if (cp >= 0x800) out.push_back(static_cast<char>((cp >= 0x10000 ? 0x80 : 0xe0) | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>((cp >= 0x800 ? 0x80 : 0xc0) | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

inline bool ParseJsonString(const std::string& raw, size_t& i, std::string& out) {
    if (i >= raw.size() || raw[i++] != '"') return false;
    out.clear();
    while (i < raw.size()) {
        const char c = raw[i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (i == raw.size()) return false;
            const char e = raw[i++];
            switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    unsigned cp;
                    if (!ParseHex4(raw, i, cp)) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (raw.compare(i, 2, "\\u") != 0) return false;
                        i += 2;
                        unsigned low;
                        if (!ParseHex4(raw, i, low) || low < 0xdc00 || low > 0xdfff) return false;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                    AppendCodepoint(out, cp);
                    break;
                }
                default: return false;
            }
        } else {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            const size_t start = --i;
            unsigned cp;
            if (!NextCodepoint(raw, i, cp)) return false;
            out.append(raw, start, i - start);
        }
    }
    return false;
}

inline void SkipWs(const std::string& raw, size_t& i) {
    while (i < raw.size() &&
           (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r')) {
        ++i;
    }
}


/** 仅接受完整对象；损坏返回空 Store，不恢复部分解析结果。 */
inline Store FromJsonObject(const std::string& raw) {
    Store store;
    size_t i = 0;
    SkipWs(raw, i);
    if (i >= raw.size() || raw[i++] != '{') return store;
    SkipWs(raw, i);
    if (i < raw.size() && raw[i] == '}') {
        ++i;
        SkipWs(raw, i);
        return store;
    }
    while (true) {
        std::string key, value;
        if (!ParseJsonString(raw, i, key)) return Store{};
        SkipWs(raw, i);
        if (i >= raw.size() || raw[i++] != ':') return Store{};
        SkipWs(raw, i);
        if (!ParseJsonString(raw, i, value)) return Store{};
        StripControlAndClamp(key, kKeyMaxChars);
        StripControlAndClamp(value, kValueMaxChars);
        if (!key.empty() && !value.empty()) {
            // 与写入一致：重复主题以最后一次为准，并保留最近 80 条。
            for (auto it = store.entries.begin(); it != store.entries.end(); ++it) {
                if (it->first == key) { store.entries.erase(it); break; }
            }
            store.entries.emplace_back(std::move(key), std::move(value));
            if (store.entries.size() > kMaxEntries) store.entries.erase(store.entries.begin());
        }
        SkipWs(raw, i);
        if (i < raw.size() && raw[i] == '}') {
            ++i;
            SkipWs(raw, i);
            return i == raw.size() ? store : Store{};
        }
        if (i >= raw.size() || raw[i++] != ',') return Store{};
        SkipWs(raw, i);
    }
}

inline void TouchMoveToBack(Store& store, size_t idx) {
    auto item = std::move(store.entries[idx]);
    store.entries.erase(store.entries.begin() + static_cast<std::ptrdiff_t>(idx));
    store.entries.push_back(std::move(item));
}

inline std::string Remember(Store& store, std::string key, std::string value) {
    StripControlAndClamp(key, kKeyMaxChars);
    StripControlAndClamp(value, kValueMaxChars);
    // 回 JSON：小智 messaging 对非 JSON 工具结果会 HTTP 500（2026-09-09 HIL）
    if (key.empty() || value.empty()) {
        return "{\"ok\":false,\"message\":\"没听清要记什么，这次没有记住。\"}";
    }
    for (size_t i = 0; i < store.entries.size(); ++i) {
        if (store.entries[i].first == key) {
            store.entries[i].second = value;
            TouchMoveToBack(store, i);
            return std::string("{\"ok\":true,\"message\":\"已记住「") + JsonEscape(key) +
                   "」。\",\"key\":\"" + JsonEscape(key) + "\"}";
        }
    }
    store.entries.emplace_back(key, value);
    while (store.entries.size() > kMaxEntries) {
        store.entries.erase(store.entries.begin());
    }
    return std::string("{\"ok\":true,\"message\":\"已记住「") + JsonEscape(key) +
           "」。\",\"key\":\"" + JsonEscape(key) + "\"}";
}


inline std::string Recall(const Store& store, std::string key) {
    StripControlAndClamp(key, kKeyMaxChars);
    size_t total = 0, budget = kRecallAllMaxChars;
    Store selected;
    bool full = false;
    for (auto it = store.entries.rbegin(); it != store.entries.rend(); ++it) {
        if (!key.empty() && it->first.find(key) == std::string::npos) continue;
        ++total;
        const size_t cost = CodepointCount(it->first) + CodepointCount(it->second);
        if (full || selected.entries.size() >= kRecallAllLimit || cost > budget) {
            full = true;
            continue;
        }
        budget -= cost;
        selected.entries.insert(selected.entries.begin(), *it);
    }
    if (!key.empty() && total == 0) {
        return std::string("{\"memories\":{},\"message\":\"没有记住关于「") + JsonEscape(key) + "」的事。\"}";
    }
    return "{\"total\":" + std::to_string(total) + ",\"returned\":" +
           std::to_string(selected.entries.size()) + ",\"memories\":" + ToJsonObject(selected) + "}";
}

inline std::string Forget(Store& store, std::string key) {
    StripControlAndClamp(key, kKeyMaxChars);
    if (key == "*") {
        store.entries.clear();
        return "{\"ok\":true,\"message\":\"已经全部忘掉了。\",\"cleared\":true}";
    }
    if (key.empty()) {
        return "{\"ok\":false,\"message\":\"没听清要忘掉什么。\"}";
    }
    for (auto it = store.entries.begin(); it != store.entries.end(); ++it) {
        if (it->first == key) {
            store.entries.erase(it);
            return std::string("{\"ok\":true,\"message\":\"已忘掉「") + JsonEscape(key) +
                   "」。\",\"key\":\"" + JsonEscape(key) + "\"}";
        }
    }
    return std::string("{\"ok\":false,\"message\":\"本来就没有记住「") + JsonEscape(key) +
           "」。\",\"key\":\"" + JsonEscape(key) + "\"}";
}

/** 序列化后若超软上限，逐出最旧直至可接受或清空。 */
inline std::string CompactToSoftMax(Store& store) {
    std::string json = ToJsonObject(store);
    while (json.size() > kJsonSoftMaxBytes && !store.entries.empty()) {
        store.entries.erase(store.entries.begin());
        json = ToJsonObject(store);
    }
    return json;
}

}  // namespace hutuji::memory

#endif  // HUTUJI_MEMORY_CORE_H
