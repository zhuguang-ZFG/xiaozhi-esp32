#ifndef HUTUJI_DRAW_BIND_H
#define HUTUJI_DRAW_BIND_H

class Display;

namespace hutuji {

/** 维护抽屉「绑定呼图账号」：屏显 QR + bind_code，HTTPS announce 到 draw-portal。 */
void StartDrawBind(Display* display);

/** 关闭绑机 QR 层（用户点关闭或绑定成功）。 */
void StopDrawBind(Display* display);

/** 一次扫码合一流程（2026-09-08 定案）：无界面 announce 轮询，portal 已有该 MAC
 * 认领时响应 bound:true 即完成绑定。在每次激活完成事件调用；幂等，窗口内重复调用
 * 安全。抽屉绑定流在跑时不抢（抽屉流的 announce 同样会被 portal 撮合）。 */
void StartAutoBindHeadless(Display* display);

/** 停止无头轮询（抽屉流开启时调用，避免双 worker 抢 announce 通道）。 */
void StopAutoBindHeadless();

bool IsDrawBindActive();

}  // namespace hutuji

#endif  // HUTUJI_DRAW_BIND_H
