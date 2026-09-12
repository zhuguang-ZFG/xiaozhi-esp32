#ifndef HUTUJI_BLE_DIAG_H
#define HUTUJI_BLE_DIAG_H

namespace hutuji::ble_diag {

/**
 * 启动 BLE-DIAG 阶段 A 只读诊断广播（枢纽 protocol.md §1.4）。
 *
 * 三重闸门，任一不成立即什么都不做并立即返回：
 *   1. 编译开关 CONFIG_HUTUJI_BLE_DIAGNOSTICS（默认 n，且 depends on BT_ENABLED）；
 *   2. 运行时 NVS `ble_diag/enabled`（缺失、非法、读失败一律按 false）；
 *   3. NimBLE controller/host 初始化与 NRPA 生成全部成功。
 *
 * 阶段 A 只有 non-connectable / non-scannable legacy advertising：不注册 GATT、
 * 不开配对路径、不附 local name / scan response、不广播稳定身份。
 *
 * fail-closed：任一步失败只关闭 BLE 并记脱敏日志，绝不阻塞或改变
 * WiFi / Telnet / job 路径，不重启整机。因此本函数不返回错误码——
 * 调用方对 BLE 结果无可执行分支。
 *
 * 未启用时是空实现，可在任何板型无条件调用。
 */
void Start();

}  // namespace hutuji::ble_diag

#endif  // HUTUJI_BLE_DIAG_H
