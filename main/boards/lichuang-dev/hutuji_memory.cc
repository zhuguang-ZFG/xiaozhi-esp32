#include "hutuji_memory.h"

#include <mutex>
#include <string>

#include "hutuji_memory_core.h"
#include "mcp_server.h"
#include "settings.h"

namespace hutuji::memory {
namespace {

constexpr const char* kNvsNs = "hutuji_mem";
constexpr const char* kNvsKey = "json";

std::mutex g_lock;

Store LoadLocked() {
    Settings settings(kNvsNs, false);
    const std::string raw = settings.GetString(kNvsKey, "{}");
    return FromJsonObject(raw);
}

void SaveLocked(Store& store) {
    const std::string json = CompactToSoftMax(store);
    Settings settings(kNvsNs, true);
    settings.SetString(kNvsKey, json);
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
            Store store = LoadLocked();
            const std::string msg = Remember(store, key, value);
            SaveLocked(store);
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
            const Store store = LoadLocked();
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
            Store store = LoadLocked();
            const std::string msg = Forget(store, key);
            SaveLocked(store);
            return msg;
        });
}

}  // namespace hutuji::memory
