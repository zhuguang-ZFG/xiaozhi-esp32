#ifndef HUTUJI_ACTIVATION_RELAY_H
#define HUTUJI_ACTIVATION_RELAY_H

#include <string>

namespace hutuji {

/**
 * 把小智 OTA 下发的激活码经京东云中转代绑控制台（2026-08-20 用户决策：
 * 「新机器联网扫码后机器码自动写入小智控制台，用户无感」；直连 serial-only
 * 预注册已实测不存在——控制台绑定端点强制 verificationCode）。
 *
 * 尽力而为：内部起一次性任务做 HTTPS POST，失败只记日志，绝不阻塞/失败化
 * 激活流程；设备屏显激活码的人工兜底始终保留。调用方不需要考虑线程安全，
 * 可重复调用（服务端幂等：已绑返回 already_bound）。
 */
void ReportActivationCode(const std::string& code);

}  // namespace hutuji

#endif  // HUTUJI_ACTIVATION_RELAY_H
