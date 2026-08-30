#ifndef HUTUJI_DRAW_BIND_H
#define HUTUJI_DRAW_BIND_H

class Display;

namespace hutuji {

/** 维护抽屉「绑定呼图账号」：屏显 QR + bind_code，HTTPS announce 到 draw-portal。 */
void StartDrawBind(Display* display);

/** 关闭绑机 QR 层（用户点关闭或绑定成功）。 */
void StopDrawBind(Display* display);

bool IsDrawBindActive();

}  // namespace hutuji

#endif  // HUTUJI_DRAW_BIND_H
