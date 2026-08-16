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
// 全脸参数：左右眉角、眉高、嘴角、嘴张合、腮红、泪滴、汗滴、火花。
// 构图参考 M5Stack-Avatar，动画机制参考 LVGL Kawaii Face；仅采用公开视觉思想，
// 不复制第三方实现。正 mouthCurve=上扬，负值=下垂；效果量归一化为 0..1。
static const FacialData kFacialMoods[] = {
    {0, 0, 0, 0, 0.22f, 0.16f, 0, 0, 0, 0},                    // neutral
    {0, 0, 0.10f, 0.10f, 0.85f, 0.18f, 0.45f, 0, 0, 0},        // happy
    {-4, -4, 0.15f, 0.15f, 1.00f, 0.75f, 0.55f, 0, 0, 0.5f},   // laughing
    {-8, 10, 0.05f, 0.16f, 0.75f, 0.35f, 0.35f, 0, 0, 0.4f},   // funny
    {-14, -14, -0.08f, -0.08f, -0.75f, 0.08f, 0, 0, 0, 0},     // sad
    {30, 30, -0.08f, -0.08f, -0.45f, 0.10f, 0, 0, 0, 0},       // angry
    {-14, -14, -0.12f, -0.12f, -0.85f, 0.35f, 0, 1, 0, 0},     // crying
    {-5, -5, 0.12f, 0.12f, 0.90f, 0.12f, 0.80f, 0, 0, 0.8f},   // loving
    {-10, 12, 0, 0.05f, 0.20f, 0.05f, 0.85f, 0, 0.25f, 0},     // embarrassed
    {0, 0, 0.20f, 0.20f, 0.05f, 0.65f, 0, 0, 0, 0},            // surprised
    {0, 0, 0.25f, 0.25f, 0, 1.00f, 0, 0, 0.65f, 0},            // shocked
    {-18, 22, 0.12f, -0.02f, -0.10f, 0.04f, 0, 0, 0, 0},       // thinking
    {-8, 12, 0.10f, 0.05f, 0.72f, 0.10f, 0.30f, 0, 0, 0.3f},   // winking
    {-4, 8, -0.02f, 0.06f, 0.22f, 0.04f, 0, 0, 0, 0.2f},       // cool
    {0, 0, -0.08f, -0.08f, 0.48f, 0.05f, 0.15f, 0, 0, 0},      // relaxed
    {-8, 10, 0.05f, 0.10f, 0.88f, 0.25f, 0.55f, 0, 0, 0.4f},   // delicious
    {-6, -6, 0.08f, 0.08f, 0.65f, 0.02f, 0.75f, 0, 0, 0.5f},   // kissy
    {8, 8, 0.06f, 0.06f, 0.38f, 0.04f, 0, 0, 0, 0},            // confident
    {0, 0, -0.18f, -0.18f, -0.08f, 0.04f, 0, 0, 0, 0},         // sleepy
    {18, -6, 0.14f, 0.03f, 0.80f, 0.40f, 0.30f, 0, 0, 0.25f},  // silly
    {20, -18, 0.12f, -0.04f, -0.22f, 0.06f, 0, 0, 0.2f, 0},    // confused
};
// 情绪配色；0 = 保持品牌青（构造色）。原则：色相只给语义强关联的情绪，同族同色系，
// 避免彩虹屏——暖金=喜悦族、红=怒、蓝系=悲、粉=爱、紫=思考、绿=平静、暗青=困、
// 琥珀=惊、橙/蜜桃=顽皮、冰蓝=酷、薰衣草=窘。
static const uint32_t kMoodColors[] = {
    0,         // neutral：品牌青
    0xFFCF3F,  // happy：暖金（喜悦族）
    0xFFCF3F,  // laughing：暖金
    0xFFCF3F,  // funny：暖金
    0x5E9BFF,  // sad：柔蓝
    0xFF453A,  // angry：红
    0x7FB3FF,  // crying：浅蓝
    0xFF7EB6,  // loving：粉
    0xD8B4E2,  // embarrassed：薰衣草
    0,         // surprised：品牌青
    0xFFD60A,  // shocked：琥珀
    0xBF8FFF,  // thinking：紫
    0xFFCF3F,  // winking：暖金
    0xBDF3FF,  // cool：冰蓝
    0x58D68D,  // relaxed：绿
    0xFF6B35,  // delicious：橙
    0xFF7EB6,  // kissy：粉
    0,         // confident：品牌青
    0x3E8E96,  // sleepy：暗青
    0xFF9770,  // silly：蜜桃
    0,         // confused：品牌青
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
    face_cur_ = face_target_ = kFacialMoods[0];
    last_frame_us_ = last_blink_us_ = esp_timer_get_time();
}

void GrobotEyes::RecomputePalette(lv_color_t eye_color) {
    scan_color_ = lv_color_mix(eye_color_, bg_color_, 7);
    glow_[0] = lv_color_mix(eye_color_, bg_color_, 38);
    glow_[1] = lv_color_mix(eye_color_, bg_color_, 77);
    glow_[2] = lv_color_mix(eye_color_, bg_color_, 128);
    iris_[0] = lv_color_mix(eye_color_, bg_color_, 153);
    iris_[1] = eye_color_;
    iris_[2] = lv_color_mix(lv_color_white(), eye_color_, 77);
    pupil_color_ = lv_color_mix(eye_color_, bg_color_, 25);
    highlight_color_ = lv_color_mix(lv_color_white(), eye_color_, 180);
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

void GrobotEyes::ApplyFacialData(const FacialData& data) { face_target_ = data; }

void GrobotEyes::SetEmotion(const char* emotion) {
    MoodData left = kMoods[0], right = kMoods[0];
    uint32_t color = 0;
    mood_index_ = -1;
    for (int i = 0; i < kMoodCount; i++) {
        if (strcmp(emotion, kNames[i]) == 0) {
            mood_index_ = i;
            left = right = kMoods[i];
            color = kMoodColors[i];
            ApplyFacialData(kFacialMoods[i]);
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
    if (mood_index_ < 0) {
        ApplyFacialData(kFacialMoods[0]);
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

uint16_t GrobotEyes::Blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
    const uint32_t r1 = (fg >> 11) & 0x1F, g1 = (fg >> 5) & 0x3F, b1 = fg & 0x1F;
    const uint32_t r2 = (bg >> 11) & 0x1F, g2 = (bg >> 5) & 0x3F, b2 = bg & 0x1F;
    const uint32_t r = (r1 * alpha + r2 * (255 - alpha)) / 255;
    const uint32_t g = (g1 * alpha + g2 * (255 - alpha)) / 255;
    const uint32_t b = (b1 * alpha + b2 * (255 - alpha)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void GrobotEyes::BufFillCircle(int cx, int cy, int r, lv_color_t c) {
    if (r <= 0)
        return;
    const uint16_t cv = lv_color_to_u16(c);
    const int s = draw_buf_->header.stride / 2;
    const float rf = (float)r;
    for (int row = std::max(0, cy - r); row <= std::min(h_ - 1, cy + r); row++) {
        const float dy = (float)(row - cy);
        const float dx_f = sqrtf(rf * rf - dy * dy);
        const int dx = (int)dx_f;
        for (int col = std::max(0, cx - dx); col <= std::min(w_ - 1, cx + dx); col++)
            buf_[row * s + col] = cv;
        // 边缘 1px 抗锯齿：按覆盖率分数与底层像素混合（不是背景色——圆会叠在
        // 辉光环/眼白上，与底色混会出现脏边）。
        const uint8_t frac = (uint8_t)((dx_f - dx) * 255.0f);
        if (frac > 0) {
            const int cl = cx - dx - 1, cr = cx + dx + 1;
            if (cl >= 0)
                buf_[row * s + cl] = Blend565(cv, buf_[row * s + cl], frac);
            if (cr < w_)
                buf_[row * s + cr] = Blend565(cv, buf_[row * s + cr], frac);
        }
    }
}
void GrobotEyes::BufFillEllipse(int cx, int cy, int rx, int ry, lv_color_t c) {
    if (rx <= 0 || ry <= 0)
        return;
    const uint16_t cv = lv_color_to_u16(c);
    for (int y = std::max(0, cy - ry); y <= std::min(h_ - 1, cy + ry); y++) {
        const float ny = (float)(y - cy) / ry;
        const int dx = (int)(rx * sqrtf(std::max(0.0f, 1.0f - ny * ny)));
        BufHLine(cx - dx, cx + dx, y, cv);
    }
}

void GrobotEyes::BufLine(int x0, int y0, int x1, int y1, lv_color_t c, int thickness) {
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        BufFillCircle(x0, y0, std::max(1, thickness), c);
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void GrobotEyes::DrawEyebrows(int cx, int cy, int eyeR, int offset) {
    const int yL = cy - eyeR - 24 - (int)(face_cur_.browLiftL * 18);
    const int yR = cy - eyeR - 24 - (int)(face_cur_.browLiftR * 18);
    const int halfW = eyeR * 2 / 3;
    const int tiltL = (int)(face_cur_.browTiltL * 0.35f);
    const int tiltR = (int)(face_cur_.browTiltR * 0.35f);
    const int browArch = 8;
    const lv_color_t brow = lv_color_mix(eye_color_, lv_color_white(), 45);
    auto drawBrow = [&](int browCx, int browY, int tilt) {
        int prevX = browCx - halfW;
        int prevY = browY - tilt;
        for (int i = 1; i <= 12; i++) {
            const float nx = -1.0f + 2.0f * i / 12.0f;
            const int x = browCx - halfW + 2 * halfW * i / 12;
            const int slope = -tilt + 2 * tilt * i / 12;
            const int y = browY + slope - (int)(browArch * (1.0f - nx * nx));
            BufLine(prevX, prevY, x, y, brow, 2);
            prevX = x;
            prevY = y;
        }
    };
    drawBrow(cx - offset, yL, tiltL);
    drawBrow(cx + offset, yR, -tiltR);
}

void GrobotEyes::DrawNose(int cx, int cy, int eyeR) {
    const int noseY = cy + eyeR / 2 + 3;
    const lv_color_t nose = lv_color_mix(eye_color_, bg_color_, 105);
    // 低对比度圆角胶囊：给全脸一个中心锚点，但不画写实鼻梁/鼻孔。
    BufFillEllipse(cx, noseY, 9, 4, nose);
    BufFillEllipse(cx, noseY - 1, 6, 2, bg_color_);
}

void GrobotEyes::DrawMouth(int cx, int cy, int eyeR) {
    const int width = eyeR + 28;
    const int curve = (int)(face_cur_.mouthCurve * 18);
    const int open = std::max(2, (int)(face_cur_.mouthOpen * 22));
    const int lipGap = std::max(3, open / 2);
    const int mouthY = cy + eyeR + 19;
    const int upperY = mouthY - lipGap / 2;
    const int lowerY = mouthY + lipGap / 2;
    const lv_color_t mouth = lv_color_mix(eye_color_, lv_color_white(), 35);
    if (open > 5) {
        const lv_color_t tongue = lv_color_hex(0xFF7EB6);
        BufFillEllipse(cx, mouthY + curve / 4, width / 2, open, mouth);
        BufFillEllipse(cx, mouthY + curve / 4, width / 2 - 3, std::max(2, open - 3), bg_color_);
        BufFillEllipse(cx, mouthY + open / 2 + curve / 4, width / 3, std::max(2, open / 3), tongue);
        return;
    }
    int prevX = cx - width / 2;
    int prevUpperY = upperY;
    int prevLowerY = lowerY;
    for (int i = 1; i <= 16; i++) {
        const float nx = -1.0f + (2.0f * i / 16.0f);
        const int x = cx - width / 2 + width * i / 16;
        const int arc = (int)(curve * (1.0f - nx * nx));
        const int nextUpperY = upperY + arc;
        const int nextLowerY = lowerY + arc / 2;
        BufLine(prevX, prevUpperY, x, nextUpperY, mouth, 2);
        BufLine(prevX, prevLowerY, x, nextLowerY, glow_[2], 1);
        prevX = x;
        prevUpperY = nextUpperY;
        prevLowerY = nextLowerY;
    }
}
void GrobotEyes::DrawFacialEffects(int cx, int cy, int eyeR, int offset) {
    const int blush = (int)(face_cur_.blush * 18);
    if (blush > 1) {
        const lv_color_t blushColor = lv_color_hex(0xFF7EB6);
        BufFillEllipse(cx - offset - eyeR, cy + eyeR / 2, blush, std::max(2, blush / 3),
                       blushColor);
        BufFillEllipse(cx + offset + eyeR, cy + eyeR / 2, blush, std::max(2, blush / 3),
                       blushColor);
    }
    const int tears = (int)(face_cur_.tears * 24);
    if (tears > 2) {
        const lv_color_t tearColor = lv_color_hex(0x7FB3FF);
        BufLine(cx - offset, cy + eyeR / 2, cx - offset - 3, cy + eyeR / 2 + tears, tearColor, 2);
        BufLine(cx + offset, cy + eyeR / 2, cx + offset + 3, cy + eyeR / 2 + tears, tearColor, 2);
        BufFillCircle(cx - offset - 3, cy + eyeR / 2 + tears, 4, tearColor);
        BufFillCircle(cx + offset + 3, cy + eyeR / 2 + tears, 4, tearColor);
    }
    if (face_cur_.sweat > 0.08f) {
        const int drop = 7 + (int)(face_cur_.sweat * 10);
        const lv_color_t sweatColor = lv_color_hex(0xBDF3FF);
        BufFillCircle(cx + offset + eyeR + 12, cy - eyeR / 2, drop / 2, sweatColor);
        BufFillTriangle(cx + offset + eyeR + 12 - drop / 2, cy - eyeR / 2,
                        cx + offset + eyeR + 12 + drop / 2, cy - eyeR / 2, cx + offset + eyeR + 12,
                        cy - eyeR / 2 + drop, sweatColor);
    }
    if (face_cur_.sparkle > 0.08f) {
        const int r = 5 + (int)(face_cur_.sparkle * 7);
        const int sx = cx + offset + eyeR + 17, sy = cy - eyeR;
        const lv_color_t sparkleColor = lv_color_hex(0xFFD60A);
        BufLine(sx - r, sy, sx + r, sy, sparkleColor, 1);
        BufLine(sx, sy - r, sx, sy + r, sparkleColor, 1);
    }
}

void GrobotEyes::BufLidArc(int cx, int cy, int eyeR, int pad, int lidH, bool top) {
    const int halfW = eyeR + pad;
    for (int x = cx - halfW; x <= cx + halfW; x++) {
        const float nx = (float)(x - cx) / halfW;
        // 抛物线：中央切到 lidH，两侧收 45%——happy(botH>0) 因此呈自然弯月，
        // 半闭眼也有眼睑弧度而不是直板。全闭（lidH>=100）保持平切，避免眨不穿。
        int cut = lidH >= 100 ? lidH : (int)(lidH * (1.0f - 0.45f * nx * nx));
        if (cut <= 0)
            continue;
        if (top)
            BufFillRect(x, cy - eyeR - pad, 1, cut + pad, bg_color_);
        else
            BufFillRect(x, cy + eyeR - cut, 1, cut + pad, bg_color_);
    }
}

void GrobotEyes::BufFillHeart(int cx, int cy, int r, lv_color_t c) {
    // 两圆一三角拼心形；r 是整体半宽。尺寸小（~12px），比例宁宽勿瘦才认得出。
    const int hr = std::max(2, r / 2);
    BufFillCircle(cx - hr, cy - hr / 2, hr, c);
    BufFillCircle(cx + hr, cy - hr / 2, hr, c);
    BufFillTriangle(cx - 2 * hr, cy - hr / 2, cx + 2 * hr, cy - hr / 2, cx, cy + r, c);
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
                         int tilt, bool isLeft, bool heartPupil) {
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
    if (heartPupil)
        BufFillHeart(pcx, pcy, std::max(5, pR * 2 / 5 + 1), pupil_color_);
    else
        BufFillCircle(pcx, pcy, std::max(3, pR * 2 / 5), pupil_color_);
    BufFillCircle(pcx - eyeR / 4, pcy - eyeR / 4, std::max(2, eyeR / 8), highlight_color_);
    int pad = 16 + abs(pulse);
    if (lidH > 0)
        BufLidArc(cx, cy, eyeR, pad, lidH, true);
    if (abs(tilt) > 0) {
        int px =
            isLeft ? ((tilt < 0) ? cx - eyeR : cx + eyeR) : ((tilt < 0) ? cx + eyeR : cx - eyeR);
        BufFillTriangle(cx - eyeR, cy - eyeR + lidH, cx + eyeR, cy - eyeR + lidH, px,
                        cy - eyeR + lidH + abs(tilt), bg_color_);
    }
    if (botH > 0)
        BufLidArc(cx, cy, eyeR, pad, botH, false);
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
    ApplySpring(face_cur_.browTiltL, face_vel_.browTiltL, face_target_.browTiltL, dt_, 150, 19);
    ApplySpring(face_cur_.browTiltR, face_vel_.browTiltR, face_target_.browTiltR, dt_, 150, 19);
    ApplySpring(face_cur_.browLiftL, face_vel_.browLiftL, face_target_.browLiftL, dt_, 150, 19);
    ApplySpring(face_cur_.browLiftR, face_vel_.browLiftR, face_target_.browLiftR, dt_, 150, 19);
    ApplySpring(face_cur_.mouthCurve, face_vel_.mouthCurve, face_target_.mouthCurve, dt_, 140, 18);
    const float mouthOpenTarget =
        speaking_
            ? 0.30f + 0.42f * fabsf(sinf((float)(last_frame_us_ % 166667) / 166667.0f * 6.2832f))
            : face_target_.mouthOpen;
    ApplySpring(face_cur_.mouthOpen, face_vel_.mouthOpen, mouthOpenTarget, dt_, 220, 24);
    ApplySpring(face_cur_.blush, face_vel_.blush, face_target_.blush, dt_, 100, 16);
    ApplySpring(face_cur_.tears, face_vel_.tears, face_target_.tears, dt_, 110, 17);
    ApplySpring(face_cur_.sweat, face_vel_.sweat, face_target_.sweat, dt_, 110, 17);
    ApplySpring(face_cur_.sparkle, face_vel_.sparkle, face_target_.sparkle, dt_, 120, 18);
    // 空闲眼跳（RoboEyes setIdleMode 惯例：间隔+随机变化量）：非说话/聆听时
    // 每 1.5~4s 视线小跳（±0.30/±0.15），停留 300~700ms 后滑回，独立软弹簧。
    if (!speaking_ && !listening_) {
        const int64_t now = esp_timer_get_time();
        if (now - last_saccade_us_ > (int64_t)saccade_interval_ms_ * 1000) {
            saccX_ = ((int)(esp_random() % 61) - 30) / 100.0f;
            saccY_ = ((int)(esp_random() % 31) - 15) / 100.0f;
            saccade_until_us_ = now + (int64_t)(300 + esp_random() % 400) * 1000;
            last_saccade_us_ = now;
            saccade_interval_ms_ = 1500 + esp_random() % 2500;
        }
        if (now > saccade_until_us_) {
            saccX_ = saccY_ = 0;
        }
    } else {
        saccX_ = saccY_ = 0;
    }
    ApplySpring(saccCurX_, saccVelX_, saccX_, dt_, 160, 20);
    ApplySpring(saccCurY_, saccVelY_, saccY_, dt_, 160, 20);
    auto s = [&](float v) { return (int)(v * scale); };
    // 说话弹跳：约 6Hz ±2px，参照 RoboEyes anim_laugh 的垂直抖动语义但收敛到
    // 微幅，避免长句时视觉疲劳；聆听时瞳孔放大 18%。
    int bounce = 0;
    if (speaking_) {
        bounce = (int)(2.0f * sinf((float)(last_frame_us_ % 166667) / 166667.0f * 6.2832f));
    }
    const float dilate = listening_ ? 1.18f : 1.0f;
    auto gaze = [&](EyeState& c, int eyeR_px, int& gx, int& gy) {
        gx = (int)((c.lookX + saccCurX_) * eyeR_px * 0.30f);
        gy = (int)((c.lookY + saccCurY_) * eyeR_px * 0.30f);
    };
    int erL = s(curL_.eyeRadius), erR = s(curR_.eyeRadius);
    int gxL, gyL, gxR, gyR;
    gaze(curL_, erL, gxL, gyL);
    gaze(curR_, erR, gxR, gyR);
    // loving(7)/kissy(16) 画心形瞳。
    const bool heart = (mood_index_ == 7 || mood_index_ == 16);
    DrawEyebrows(cx, cy + bounce, std::max(erL, erR), offset);
    DrawEye(cx - offset, cy + bounce, gxL, gyL, (int)(s(curL_.pR) * dilate), erL, s(curL_.topH),
            s(curL_.botH), s(curL_.tilt), true, heart);
    DrawEye(cx + offset, cy + bounce, gxR, gyR, (int)(s(curR_.pR) * dilate), erR, s(curR_.topH),
            s(curR_.botH), s(curR_.tilt), false, heart);
    DrawNose(cx, cy + bounce, std::max(erL, erR));
    DrawFacialEffects(cx, cy + bounce, std::max(erL, erR), offset);
    DrawMouth(cx, cy + bounce, std::max(erL, erR));
    lv_obj_invalidate(canvas_);
}
