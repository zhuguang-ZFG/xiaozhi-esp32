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
};

struct EyeState {
    float topH, botH, tilt, pR, eyeRadius;
    float vTop, vBot, vTilt, vPR, vRadius;
};

class GrobotEyes {
public:
    GrobotEyes(lv_color_t eyeColor, lv_color_t bgColor);
    ~GrobotEyes();
    void Init(lv_obj_t* parent, int w, int h);
    void SetEmotion(const char* emotion);

private:
    static void TimerCb(lv_timer_t* t);
    void Render();
    void UpdateDeltaTime();
    void Blink();
    void ApplySpring(float& cur, float& vel, float target, float dt,
                     float stiffness = 180.0f, float damping = 15.0f);
    void DrawEye(int cx, int cy, int pR, int eyeR, int lidH, int botH,
                 int tilt, bool isLeft);
    void DrawBackground();
    void BufFillRect(int x, int y, int w, int h, lv_color_t c);
    void BufFillCircle(int cx, int cy, int r, lv_color_t c);
    void BufFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                         lv_color_t c);
    void BufHLine(int x0, int x1, int y, uint16_t cv);

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

    EyeState curL_{}, curR_{}, targetL_{}, targetR_{}, baseL_{}, baseR_{};

    int64_t last_frame_us_ = 0;
    int64_t last_blink_us_ = 0;
    int blink_interval_ms_ = 3000;
    int blink_dur_ms_ = 250;
    float dt_ = 0;
    int frame_ = 0;

    static constexpr float kEyeRadius = 45.0f;
    static constexpr float kPupilRadius = 30.0f;
    static constexpr int kEyeGap = 20;
    static constexpr float kBaseH = 120.0f;
};

#endif
