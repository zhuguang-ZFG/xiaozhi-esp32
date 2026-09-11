#ifndef HUTUJI_MEMORY_H
#define HUTUJI_MEMORY_H

class McpServer;

namespace hutuji::memory {

/** 注册 hutuji.remember / recall / forget（本机 NVS，用户无感隔离）。 */
void RegisterTools(McpServer& mcp_server);

}  // namespace hutuji::memory

#endif  // HUTUJI_MEMORY_H
