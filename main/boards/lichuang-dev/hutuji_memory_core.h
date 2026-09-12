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

inline void StripControlAndClamp(std::string& text, size_t limit) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (c < 0x20u || c == 0x7fu) {
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    size_t start = 0;
    while (start < out.size() && (out[start] == ' ' || out[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        out.erase(0, start);
    }
    if (out.size() > limit) {
        out.resize(limit);
    }
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

inline bool ParseJsonString(const std::string& raw, size_t& i, std::string& out) {
    if (i >= raw.size() || raw[i] != '"') {
        return false;
    }
    ++i;
    out.clear();
    while (i < raw.size()) {
        char c = raw[i++];
        if (c == '"') {
            return true;
        }
        if (c == '\\' && i < raw.size()) {
            char e = raw[i++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    // 跳过 \uXXXX（四位）；失败则整段拒
                    if (i + 4 > raw.size()) {
                        return false;
                    }
                    i += 4;
                    out.push_back('?');
                    break;
                default:
                    return false;
            }
            continue;
        }
        out.push_back(c);
    }
    return false;
}

inline void SkipWs(const std::string& raw, size_t& i) {
    while (i < raw.size() &&
           (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r')) {
        ++i;
    }
}

/** 仅接受 {"k":"v",...}；损坏 → 空 Store（fail-open）。 */
inline Store FromJsonObject(const std::string& raw) {
    Store store;
    size_t i = 0;
    SkipWs(raw, i);
    if (i >= raw.size() || raw[i] != '{') {
        return store;
    }
    ++i;
    while (true) {
        SkipWs(raw, i);
        if (i < raw.size() && raw[i] == '}') {
            return store;
        }
        std::string key;
        if (!ParseJsonString(raw, i, key)) {
            return Store{};
        }
        SkipWs(raw, i);
        if (i >= raw.size() || raw[i] != ':') {
            return Store{};
        }
        ++i;
        SkipWs(raw, i);
        std::string value;
        if (!ParseJsonString(raw, i, value)) {
            return Store{};
        }
        if (!key.empty() && !value.empty()) {
            store.entries.emplace_back(std::move(key), std::move(value));
        }
        SkipWs(raw, i);
        if (i < raw.size() && raw[i] == ',') {
            ++i;
            continue;
        }
        if (i < raw.size() && raw[i] == '}') {
            return store;
        }
        return Store{};
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
    if (key.empty()) {
        const size_t total = store.entries.size();
        std::vector<std::pair<std::string, std::string>> items;
        size_t budget = kRecallAllMaxChars;
        for (auto it = store.entries.rbegin(); it != store.entries.rend(); ++it) {
            if (items.size() >= kRecallAllLimit) {
                break;
            }
            const size_t cost = it->first.size() + it->second.size();
            if (!items.empty() && cost > budget) {
                break;
            }
            budget = cost > budget ? 0 : budget - cost;
            items.push_back(*it);
        }
        // 最新在后 → 输出按写入序
        std::string out = "{\"total\":";
        out += std::to_string(total);
        out += ",\"returned\":";
        out += std::to_string(items.size());
        out += ",\"memories\":{";
        bool first = true;
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += '"';
            out += JsonEscape(it->first);
            out += "\":\"";
            out += JsonEscape(it->second);
            out += '"';
        }
        out += "}}";
        return out;
    }
    std::string hits = "{";
    bool first = true;
    for (const auto& kv : store.entries) {
        if (kv.first.find(key) == std::string::npos) {
            continue;
        }
        if (!first) {
            hits += ',';
        }
        first = false;
        hits += '"';
        hits += JsonEscape(kv.first);
        hits += "\":\"";
        hits += JsonEscape(kv.second);
        hits += '"';
    }
    hits += '}';
    if (first) {
        return std::string("{\"memories\":{},\"message\":\"没有记住关于「") + JsonEscape(key) +
               "」的事。\"}";
    }
    return std::string("{\"memories\":") + hits + "}";
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
