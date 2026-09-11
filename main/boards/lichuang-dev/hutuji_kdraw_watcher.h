#ifndef HUTUJI_KDRAW_WATCHER_H
#define HUTUJI_KDRAW_WATCHER_H

namespace hutuji {
namespace kdraw {

/**
 * @brief 奎享完成守护（protocol §9-H′ 的观察端）。
 *
 * Grbl_Esp32 侧 StatusBeacon 以 UDP 广播 `<state|Changing=On|Seq=N>`（:2325）。
 * 本模块监听并判定「奎享画完」：见到 Run 后，Idle 且非换纸（Changing=On）
 * 连续稳定 ≥2.5s 视为完成——本地提示音 + 屏显，并经 Job::Notify 推云端通知。
 * UDP 无连接，奎享占 Telnet/串口零影响；写字机不广播（旧固件/未开宏）时静默。
 */
void Start();

}  // namespace kdraw
}  // namespace hutuji

#endif  // HUTUJI_KDRAW_WATCHER_H
