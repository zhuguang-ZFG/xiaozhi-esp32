#include "grobot_eyes.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// 每情绪 7 参：topH, botH, tilt, pR, radius, lookX, lookY。
// 视线取值参考社区 RoboEyes setPosition() 方位语义：thinking 看右上、sad 垂眼、
// cool 侧视、embarrassed 移开视线；正脸情绪保持居中。
static const MoodData kMoods[] = {
    {0, 0, 0, 30, 45, 0, 0},               // neutral
    {0, 50, 0, 30, 45, 0, -0.10f},         // happy
    {10, 65, 0, 25, 45, 0, -0.15f},        // laughing
    {5, 55, 0, 28, 45, 0.15f, 0},          // funny
    {0, 0, -60, 27, 45, 0, 0.35f},         // sad（垂眼向下）
    {0, 0, 60, 30, 45, 0, -0.05f},         // angry（瞪视）
    {20, 0, -50, 22, 42, 0, 0.30f},        // crying
    {0, 45, 0, 32, 48, 0, -0.10f},         // loving
    {25, 15, 0, 22, 40, -0.25f, 0.25f},    // embarrassed（移开视线）
    {0, 0, 0, 38, 52, 0, 0},               // surprised（居中放大）
    {0, 0, -10, 42, 55, 0, 0},             // shocked
    {10, 15, -20, 28, 43, 0.45f, -0.40f},  // thinking（看右上）
    {0, 35, 25, 0, 45, 0.15f, 0},          // winking
    {15, 20, 10, 28, 45, 0.40f, 0},        // cool（侧视）
    {35, 0, 0, 25, 43, 0, 0.10f},          // relaxed
    {5, 60, 0, 28, 45, 0.20f, -0.20f},     // delicious
    {0, 50, 0, 18, 40, 0, -0.05f},         // kissy
    {10, 15, 15, 30, 45, 0.25f, -0.10f},   // confident
    {55, 0, 0, 20, 42, 0, 0.30f},          // sleepy
    {0, 40, 0, 30, 45, -0.15f, 0},         // silly
    {5, 0, -30, 25, 43, -0.30f, 0},        // confused
};
// 情绪配色；0 = 保持构造色（青）。远处可读性是目的，只给辨识度收益大的情绪换色。
static const uint32_t kMoodColors[] = {
    0,         // neutral
    0,         // happy
    0,         // laughing
    0,         // funny
    0x5E9BFF,  // sad：柔蓝
    0xFF453A,  // angry：红
    0x7FB3FF,  // crying：浅蓝
    0xFF7EB6,  // loving：粉
    0,         // embarrassed
    0,         // surprised
    0xFFD60A,  // shocked：琥珀
    0xBF8FFF,  // thinking：紫
    0,         // winking
    0,         // cool
    0x58D68D,  // relaxed：绿
    0,         // delicious
    0xFF7EB6,  // kissy：粉
    0,         // confident
    0x3E8E96,  // sleepy：暗青
    0,         // silly
    0,         // confused
};
static const char* kNames[] = {
    "neutral", "happy",       "laughing",  "funny",     "sad",      "angry",   "crying",
    "loving",  "embarrassed", "surprised", "shocked",   "thinking", "winking", "cool",
    "relaxed", "delicious",   "kissy",     "confident", "sleepy",   "silly",   "confused",
};
static constexpr int kMoodCount = 21;

GrobotEyes::GrobotEyes(lv_color_t eyeColor, lv_color_t bgColor)
    : eye_color_(eyeColor), bg_color_(bgColor), eye_color_default_(eyeColor) {
    RecomputePalette(eyeColor);
    curL_ = {0, 0, 0, kPupilRadius, kEyeRadius, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    curR_ = targetL_ = targetR_ = baseL_ = baseR_ = curL_;
    last_frame_us_ = last_blink_us_ = esp_timer_get_time();
}

void GrobotEyes::RecomputePalette(lv_color_t eye_color) {
    eye_color_ = eye_color;
    glow_[0] = lv_color_mix(eye_color_, bg_color_, 38);
    glow_[1] = lv_color_mix(eye_color_, bg_color_, 77);
    glow_[2] = lv_color_mix(eye_color_, bg_color_, 128);
    iris_[0] = lv_color_mix(eye_color_, bg_color_, 153);
    iris_[1] = eye_color_;
    iris_[2] = lv_color_mix(lv_color_white(), eye_color_, 77);
    pupil_color_ = lv_color_mix(eye_color_, bg_color_, 25);
    highlight_color_ = lv_color_mix(lv_color_white(), eye_color_, 180);
    scan_color_ = lv_color_mix(eye_color_, bg_color_, 12);
}

GrobotEyes::~GrobotEyes() {
    if (timer_)
        lv_timer_delete(timer_);
    if (canvas_)
        lv_obj_delete(canvas_);
    if (draw_buf_)
        lv_draw_buf_destroy(draw_buf_);
}

void GrobotEyes::Init(lv_obj_t* parent, int w, int h) {
    w_ = w;
    h_ = h;
    draw_buf_ = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
    canvas_ = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(canvas_, draw_buf_);
    lv_obj_center(canvas_);
    buf_ = (uint16_t*)draw_buf_->data;
    timer_ = lv_timer_create(TimerCb, 33, this);
}

void GrobotEyes::TimerCb(lv_timer_t* t) { ((GrobotEyes*)lv_timer_get_user_data(t))->Render(); }

void GrobotEyes::ApplySpring(float& cur, float& vel, float target, float dt, float stiffness,
                             float damping) {
    vel += ((target - cur) * stiffness - vel * damping) * dt;
    cur += vel * dt;
}

void GrobotEyes::UpdateDeltaTime() {
    int64_t now = esp_timer_get_time();
    dt_ = std::clamp((now - last_frame_us_) / 1e6f, 0.0001f, 0.05f);
    last_frame_us_ = now;
}

void GrobotEyes::Blink() {
    int64_t elapsed = (esp_timer_get_time() - last_blink_us_) / 1000;
    if (elapsed > blink_interval_ms_)
        targetL_.topH = targetR_.topH = 100;
    if (elapsed > blink_interval_ms_ + blink_dur_ms_) {
        targetL_.topH = baseL_.topH;
        targetR_.topH = baseR_.topH;
        last_blink_us_ = esp_timer_get_time();
        blink_dur_ms_ = 200 + (esp_random() % 200);
        blink_interval_ms_ = 2000 + (esp_random() % 4000);
    }
}

void GrobotEyes::SetEmotion(const char* emotion) {
    MoodData left = kMoods[0], right = kMoods[0];
    uint32_t color = 0;
    for (int i = 0; i < kMoodCount; i++) {
        if (strcmp(emotion, kNames[i]) == 0) {
            left = right = kMoods[i];
            color = kMoodColors[i];
            if (i == 11) {
                right = {15, 20, 15, 25, 42, 0.45f, -0.40f};  // thinking 右眼稍眯同向看
            } else if (i == 12) {
                // winking 修复：此前左右眼同 MoodData，根本不眨；右眼全闭（topH=100
                // 与 Blink() 的全闭量一致），左眼保持 kMoods 的开心微眯。
                right = {100, 0, 0, 25, 45, 0.15f, 0};
            } else if (i == 19) {
                left = {0, 35, 25, 0, 45, -0.15f, 0};
                right = {0, 50, 0, 30, 45, -0.15f, 0};
            }
            break;
        }
    }
    auto set = [](EyeState& b, EyeState& t, const MoodData& m) {
        b = t = {m.topH, m.botH, m.tilt, m.pR, m.radius, m.lookX, m.lookY, 0, 0, 0, 0, 0, 0, 0};
    };
    set(baseL_, targetL_, left);
    set(baseR_, targetR_, right);
    RecomputePalette(color != 0 ? lv_color_hex(color) : eye_color_default_);
}

void GrobotEyes::SetSpeaking(bool on) { speaking_ = on; }

void GrobotEyes::SetListening(bool on) { listening_ = on; }

void GrobotEyes::BufHLine(int x0, int x1, int y, uint16_t cv) {
    if (y < 0 || y >= h_)
        return;
    int s = draw_buf_->header.stride / 2;
    for (int x = std::max(0, x0); x <= std::min(w_ - 1, x1); x++)
        buf_[y * s + x] = cv;
}

void GrobotEyes::BufFillRect(int x, int y, int w, int h, lv_color_t c) {
    uint16_t cv = lv_color_to_u16(c);
    int s = draw_buf_->header.stride / 2;
    for (int r = std::max(0, y); r < std::min(h_, y + h); r++)
        for (int col = std::max(0, x); col < std::min(w_, x + w); col++)
            buf_[r * s + col] = cv;
}

void GrobotEyes::BufFillCircle(int cx, int cy, int r, lv_color_t c) {
    if (r <= 0)
        return;
    uint16_t cv = lv_color_to_u16(c);
    int s = draw_buf_->header.stride / 2, r2 = r * r;
    for (int row = std::max(0, cy - r); row <= std::min(h_ - 1, cy + r); row++) {
        int dy = row - cy, dx = (int)sqrtf((float)(r2 - dy * dy));
        for (int col = std::max(0, cx - dx); col <= std::min(w_ - 1, cx + dx); col++)
            buf_[row * s + col] = cv;
    }
}

void GrobotEyes::BufFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c) {
    uint16_t cv = lv_color_to_u16(c);
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    if (y0 > y2) {
        std::swap(x0, x2);
        std::swap(y0, y2);
    }
    if (y1 > y2) {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    int total = y2 - y0;
    if (!total)
        return;
    for (int i = 0; i <= total; i++) {
        int y = y0 + i;
        float alpha = (float)i / total;
        int xa = x0 + (int)((x2 - x0) * alpha), xb;
        if (y < y1) {
            int seg = y1 - y0;
            xb = x0 + (seg ? (int)((x1 - x0) * (float)(y - y0) / seg) : (x1 - x0));
        } else {
            int seg = y2 - y1;
            xb = x1 + (seg ? (int)((x2 - x1) * (float)(y - y1) / seg) : (x2 - x1));
        }
        BufHLine(std::min(xa, xb), std::max(xa, xb), y, cv);
    }
}

void GrobotEyes::DrawBackground() {
    lv_canvas_fill_bg(canvas_, bg_color_, LV_OPA_COVER);
    buf_ = (uint16_t*)draw_buf_->data;
    uint16_t sc = lv_color_to_u16(scan_color_);
    for (int y = 3; y < h_; y += 4)
        BufHLine(0, w_ - 1, y, sc);
    BufHLine(0, w_ - 1, h_ / 2, lv_color_to_u16(glow_[0]));
    int b = 12;
    BufFillRect(0, 0, b, 1, glow_[1]);
    BufFillRect(0, 0, 1, b, glow_[1]);
    BufFillRect(w_ - b, 0, b, 1, glow_[1]);
    BufFillRect(w_ - 1, 0, 1, b, glow_[1]);
    BufFillRect(0, h_ - 1, b, 1, glow_[1]);
    BufFillRect(0, h_ - b, 1, b, glow_[1]);
    BufFillRect(w_ - b, h_ - 1, b, 1, glow_[1]);
    BufFillRect(w_ - 1, h_ - b, 1, b, glow_[1]);
}

void GrobotEyes::DrawEye(int cx, int cy, int gazeX, int gazeY, int pR, int eyeR, int lidH, int botH,
                         int tilt, bool isLeft) {
    float phase = (float)(last_frame_us_ % 10000000) / 10000000.0f * 6.2832f;
    int pulse = (int)(2.0f * sinf(phase));
    BufFillCircle(cx, cy, eyeR + 15 + pulse, glow_[0]);
    BufFillCircle(cx, cy, eyeR + 14 + pulse, bg_color_);
    BufFillCircle(cx, cy, eyeR + 10, glow_[0]);
    BufFillCircle(cx, cy, eyeR + 6, glow_[1]);
    BufFillCircle(cx, cy, eyeR + 3, glow_[2]);
    BufFillCircle(cx, cy, eyeR, eye_color_);
    // 眼球（虹膜+瞳+高光）随视线偏移；辉光环与眼睑遮罩保持原位，视线偏移上界
    // 在 Render 侧按 eyeR 比例限制，虹膜不会跑出眼白。
    int pcx = cx + gazeX, pcy = cy + gazeY;
    int ir = eyeR * 3 / 4;
    BufFillCircle(pcx, pcy, ir, iris_[0]);
    BufFillCircle(pcx, pcy, ir * 3 / 4, iris_[1]);
    BufFillCircle(pcx, pcy, ir / 2, iris_[2]);
    BufFillCircle(pcx, pcy, std::max(3, pR * 2 / 5), pupil_color_);
    BufFillCircle(pcx - eyeR / 4, pcy - eyeR / 4, std::max(2, eyeR / 8), highlight_color_);
    int pad = 16 + abs(pulse);
    if (lidH > 0)
        BufFillRect(cx - eyeR - pad, cy - eyeR - pad, (eyeR + pad) * 2, lidH + pad, bg_color_);
    if (abs(tilt) > 0) {
        int px =
            isLeft ? ((tilt < 0) ? cx - eyeR : cx + eyeR) : ((tilt < 0) ? cx + eyeR : cx - eyeR);
        BufFillTriangle(cx - eyeR, cy - eyeR + lidH, cx + eyeR, cy - eyeR + lidH, px,
                        cy - eyeR + lidH + abs(tilt), bg_color_);
    }
    if (botH > 0)
        BufFillRect(cx - eyeR - pad, cy + eyeR - botH, (eyeR + pad) * 2, botH + pad, bg_color_);
}

void GrobotEyes::Render() {
    UpdateDeltaTime();
    DrawBackground();
    Blink();
    float scale = (float)h_ / kBaseH;
    int offset = (int)(kEyeRadius + kEyeGap) * scale;
    int cx = w_ / 2, cy = h_ / 2;
    auto spring = [&](EyeState& c, EyeState& t) {
        ApplySpring(c.topH, c.vTop, t.topH, dt_);
        ApplySpring(c.botH, c.vBot, t.botH, dt_);
        ApplySpring(c.tilt, c.vTilt, t.tilt, dt_);
        ApplySpring(c.pR, c.vPR, t.pR, dt_, 180, 22);
        ApplySpring(c.eyeRadius, c.vRadius, t.eyeRadius, dt_);
        // 视线用更软的弹簧（120/18）：扫视是滑过去的，不是弹过去的。
        ApplySpring(c.lookX, c.vLookX, t.lookX, dt_, 120, 18);
        ApplySpring(c.lookY, c.vLookY, t.lookY, dt_, 120, 18);
    };
    spring(curL_, targetL_);
    spring(curR_, targetR_);
    auto s = [&](float v) { return (int)(v * scale); };
    // 说话弹跳：约 6Hz ±2px，参照 RoboEyes anim_laugh 的垂直抖动语义但收敛到
    // 微幅，避免长句时视觉疲劳；聆听时瞳孔放大 18%。
    int bounce = 0;
    if (speaking_) {
        bounce = (int)(2.0f * sinf((float)(last_frame_us_ % 166667) / 166667.0f * 6.2832f));
    }
    const float dilate = listening_ ? 1.18f : 1.0f;
    auto gaze = [&](EyeState& c, int eyeR_px, int& gx, int& gy) {
        gx = (int)(c.lookX * eyeR_px * 0.30f);
        gy = (int)(c.lookY * eyeR_px * 0.30f);
    };
    int erL = s(curL_.eyeRadius), erR = s(curR_.eyeRadius);
    int gxL, gyL, gxR, gyR;
    gaze(curL_, erL, gxL, gyL);
    gaze(curR_, erR, gxR, gyR);
    DrawEye(cx - offset, cy + bounce, gxL, gyL, (int)(s(curL_.pR) * dilate), erL, s(curL_.topH),
            s(curL_.botH), s(curL_.tilt), true);
    DrawEye(cx + offset, cy + bounce, gxR, gyR, (int)(s(curR_.pR) * dilate), erR, s(curR_.topH),
            s(curR_.botH), s(curR_.tilt), false);
    lv_obj_invalidate(canvas_);
}
