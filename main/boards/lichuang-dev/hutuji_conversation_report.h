#ifndef HUTUJI_CONVERSATION_REPORT_H
#define HUTUJI_CONVERSATION_REPORT_H

#include <string>

namespace hutuji {

/**
 * 对话 turn 上报（链 1）：与 Application 层 >> / << 日志同源，异步 POST 到
 * draw-portal。仅已绑定呼图账号的设备 MAC 会被服务端接受。
 *
 * 尽力而为：RAM 环形队列 + 失败丢弃；不阻塞语音主链路。
 * 须在板级 InitializeTools() 尽早调用 InitConversationReport()，用静态栈
 * 预建 worker，避免对话高峰期 heap 凑不出 TLS 任务栈。
 */
void InitConversationReport();

void ReportConversationTurn(const char* role, const char* content,
                            const std::string& session_id);

}  // namespace hutuji

#endif  // HUTUJI_CONVERSATION_REPORT_H
