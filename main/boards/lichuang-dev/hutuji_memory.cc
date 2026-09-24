#include "hutuji_memory.h"

#include <mutex>
#include <string>

#include "hutuji_memory_core.h"
#include "mcp_server.h"
#include <nvs.h>

namespace hutuji::memory {
namespace {

constexpr const char* kNvsNs = "hutuji_mem";
constexpr const char* kNvsKey = "json";

std::mutex g_lock;

struct NvsHandle {
    nvs_handle_t value = 0;
    ~NvsHandle() { if (value != 0) nvs_close(value); }
};

std::string StorageError() {
    return "{\"ok\":false,\"message\":\"本机记忆存储暂不可用，请稍后重试。\"}";
}

bool LoadLocked(Store& store) {
    NvsHandle handle;
    esp_err_t result = nvs_open(kNvsNs, NVS_READONLY, &handle.value);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    size_t length = 0;
    result = nvs_get_str(handle.value, kNvsKey, nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    // 长度含末尾 NUL；拒绝异常长度，避免故障值挤占设备内存。
    if (result != ESP_OK || length == 0 || length > kJsonSoftMaxBytes + 1) return false;
    std::string raw(length, '\0');
    result = nvs_get_str(handle.value, kNvsKey, raw.data(), &length);
    if (result != ESP_OK || raw.back() != '\0') return false;
    raw.pop_back();
    store = FromJsonObject(raw);
    return true;
}

bool SaveLocked(Store& store) {
    const std::string json = CompactToSoftMax(store);
    NvsHandle handle;
    if (nvs_open(kNvsNs, NVS_READWRITE, &handle.value) != ESP_OK) return false;
    if (nvs_set_str(handle.value, kNvsKey, json.c_str()) != ESP_OK) return false;
    // 不经 ESP_ERROR_CHECK；持久化失败不能白屏重启或回报记忆成功。
    return nvs_commit(handle.value) == ESP_OK;
}

}  // namespace

void RegisterTools(McpServer& mcp_server) {
    mcp_server.AddTool(
        "hutuji.remember",
        "记住用户告诉你的事情（称呼、偏好、约定等），保存在本机。"
        "key 是简短主题（≤50 字），value 是内容（≤500 字），同主题覆盖旧的。"
        "用户说「记住…」「以后都…」时用；密码、验证码等敏感信息不要记。"
        "每台设备各自一份记忆，不要改用云端 hutuji_remember。",
        PropertyList({Property("key", kPropertyTypeString), Property("value", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string key = properties["key"].value<std::string>();
            const std::string value = properties["value"].value<std::string>();
            std::lock_guard<std::mutex> lock(g_lock);
            Store store;
            if (!LoadLocked(store)) return StorageError();
            const auto before = store.entries;
            const std::string msg = Remember(store, key, value);
            if (store.entries != before && !SaveLocked(store)) return StorageError();
            return msg;
        });

    mcp_server.AddTool(
        "hutuji.recall",
        "回忆本机记住的事情。key 留空返回全部（很多时只回最近的并注明总数）；"
        "给 key 按主题匹配。用户问「我说过…吗」「你记得…吗」时先查它再回答；"
        "查不到就老实说没记住，不要编。不要改用云端 hutuji_recall。",
        PropertyList({Property("key", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string key = properties["key"].value<std::string>();
            std::lock_guard<std::mutex> lock(g_lock);
            Store store;
            if (!LoadLocked(store)) return StorageError();
            return Recall(store, key);
        });

    mcp_server.AddTool(
        "hutuji.forget",
        "忘掉本机记住的事情：key 精确删除；传 * 清空本机全部（须用户明确要求）。"
        "用户说「忘掉…」「别记了」时用。不要改用云端 hutuji_forget。",
        PropertyList({Property("key", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string key = properties["key"].value<std::string>();
            std::lock_guard<std::mutex> lock(g_lock);
            Store store;
            if (!LoadLocked(store)) return StorageError();
            const auto before = store.entries;
            const std::string msg = Forget(store, key);
            if (store.entries != before && !SaveLocked(store)) return StorageError();
            return msg;
        });
}

}  // namespace hutuji::memory
