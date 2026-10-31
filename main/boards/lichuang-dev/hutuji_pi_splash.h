#ifndef HUTUJI_PI_SPLASH_H
#define HUTUJI_PI_SPLASH_H

#include <lvgl.h>

/**
 * 开机启动画面：一比一复刻 omp CLI 的 π logo 入场动画（3000ms，上游原值），
 * 结束后 500ms 把画面交接给 Grobot 脸（π 上浮淡出，眼睛淡入上浮）。
 *
 * 字形/配色/相位公式全部来自 `hutuji_pi_splash_core.h`（逐字复刻上游常量），
 * 本类只负责「字符单元 → 屏幕像素」的光栅化与 LVGL 生命周期。
 */
class HutujiPiSplash {
public:
    HutujiPiSplash() = default;
    ~HutujiPiSplash();

    /**
     * 在 `parent` 上铺一层不透明底 + logo 画布并开始播放。
     * @param reveal_target 交接阶段淡入上浮的对象（Grobot 脸容器）；可为 nullptr。
     * @return false 表示分配失败，调用方应直接跳过启动画面。
     */
    bool Start(lv_obj_t* parent, int screen_w, int screen_h, lv_obj_t* reveal_target);

    /** 是否仍在播放（含交接阶段）。 */
    bool active() const { return timer_ != nullptr; }

private:
    static void TimerCb(lv_timer_t* t);
    void Tick();
    void RenderLogo(float phase, float shine_strength, float shine_pos);
    void FillRect(int x, int y, int w, int h, uint16_t color);
    void Finish();

    lv_obj_t* backdrop_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_draw_buf_t* draw_buf_ = nullptr;
    uint16_t* buf_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t* reveal_target_ = nullptr;
    int logo_w_ = 0;
    int logo_h_ = 0;
    int cell_w_ = 0;
    int cell_h_ = 0;
    int64_t start_us_ = 0;
};

#endif  // HUTUJI_PI_SPLASH_H
