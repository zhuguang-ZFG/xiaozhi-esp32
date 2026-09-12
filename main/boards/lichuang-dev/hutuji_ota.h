#ifndef HUTUJI_OTA_H
#define HUTUJI_OTA_H

class McpServer;

namespace hutuji::ota {

/**
 * 注册 `hutuji.ota_check` / `hutuji.ota_start`。
 * 须在板级 InitializeTools() 内调用（与其它 hutuji.* 工具同栈）。
 */
void RegisterTools(McpServer& mcp_server);

/**
 * 每天 idle+WiFi 自检：只 GET latest.json、设 update_available、轻提示；
 * **禁止**自动 ota_start。跨日 NVS `hutuji_ota/last_day`；可从 Application
 * 时钟 tick 廉价调用（内部早退）。
 */
void MaybeDailyCheck();

}  // namespace hutuji::ota

#endif  // HUTUJI_OTA_H
