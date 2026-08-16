#ifndef GROBOT_EYES_H
#define GROBOT_EYES_H

#include <lvgl.h>
#include <cstdint>

struct MoodData {
    float topH;
    float botH;
    float tilt;
    float pR;
    float radius;
    // 归一化视线方向（-1..1；lookX>0 向右，lookY>0 向下），语义对齐社区
    // FluxGarage/RoboEyes 的 setPosition() 方位参数。
    float lookX;
    float lookY;
};

struct EyeState {
    float topH, botH, tilt, pR, eyeRadius;
    float lookX, lookY;
    float vTop, vBot, vTilt, vPR, vRadius, vLookX, vLookY;
};

class GrobotEyes {
public:
    GrobotEyes(lv_color_t eyeColor, lv_color_t bgColor);
    ~GrobotEyes();
    void Init(lv_obj_t* parent, int w, int h);
    void SetEmotion(const char* emotion);
    /** 说话中：双眼随语速微弹跳（幅度 ±2px，约 6Hz）。 */
    void SetSpeaking(bool on);
    /** 聆听中：瞳孔放大 ~18%（专注倾听的视觉反馈）。 */
    void SetListening(bool on);

private:
    static void TimerCb(lv_timer_t* t);
    void Render();
    void UpdateDeltaTime();
    void Blink();
    void ApplySpring(float& cur, float& vel, float target, float dt, float stiffness = 180.0f,
                     float damping = 15.0f);
    void DrawEye(int cx, int cy, int gazeX, int gazeY, int pR, int eyeR, int lidH, int botH,
                 int tilt, bool isLeft, bool heartPupil);
    void DrawBackground();
    void BufFillRect(int x, int y, int w, int h, lv_color_t c);
    void BufFillCircle(int cx, int cy, int r, lv_color_t c);
    void BufFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c);
    void BufHLine(int x0, int x1, int y, uint16_t cv);
    /** 弧线眼睑：逐列抛物线切割（中央最高、边缘收 45%），替代直边矩形。 */
    void BufLidArc(int cx, int cy, int eyeR, int pad, int lidH, bool top);
    /** 心形瞳：两圆一三角拼装，loving/kissy 专用。 */
    void BufFillHeart(int cx, int cy, int r, lv_color_t c);
    /** RGB565 空间按 alpha 混合（圆边抗锯齿用，与底层像素混合而非背景色）。 */
    static uint16_t Blend565(uint16_t fg, uint16_t bg, uint8_t alpha);

    void RecomputePalette(lv_color_t eye_color);

    lv_obj_t* canvas_ = nullptr;
    lv_draw_buf_t* draw_buf_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint16_t* buf_ = nullptr;
    int w_ = 0, h_ = 0;

    lv_color_t eye_color_, bg_color_;
    lv_color_t glow_[3];
    lv_color_t iris_[3];
    lv_color_t pupil_color_;
    lv_color_t highlight_color_;
    lv_color_t scan_color_;
    lv_color_t eye_color_default_;
    bool speaking_ = false;
    bool listening_ = false;
    // 当前情绪索引（loving=7/kissy=16 画心形瞳）；未识别情绪为 -1。
    int mood_index_ = 0;
    // 空闲眼跳（RoboEyes setIdleMode 语义：间隔+随机变化量）：非说话/聆听时
    // 视线每 1.5~4s 小跳一次并滑回，经独立软弹簧避免跳切。
    float saccX_ = 0, saccY_ = 0;
    float saccCurX_ = 0, saccCurY_ = 0, saccVelX_ = 0, saccVelY_ = 0;
    int64_t last_saccade_us_ = 0;
    int64_t saccade_until_us_ = 0;
    int saccade_interval_ms_ = 2000;

    EyeState curL_{}, curR_{}, targetL_{}, targetR_{}, baseL_{}, baseR_{};

    int64_t last_frame_us_ = 0;
    int64_t last_blink_us_ = 0;
    int blink_interval_ms_ = 3000;
    int blink_dur_ms_ = 250;
    float dt_ = 0;

    static constexpr float kEyeRadius = 45.0f;
    static constexpr float kPupilRadius = 30.0f;
    static constexpr int kEyeGap = 20;
    static constexpr float kBaseH = 120.0f;
};

#endif
