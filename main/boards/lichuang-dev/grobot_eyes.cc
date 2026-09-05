#include "grobot_eyes.h"
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

// 情绪配色与开机 π logo 共用同一条渐变，色标定义只此一处。
#include "hutuji_pi_splash_core.h"

static const char* TAG = "GrobotEyes";

// 每情绪 7 参：topH, botH, tilt, pR, radius, lookX, lookY。
// 视线取值参考社区 RoboEyes setPosition() 方位语义：thinking 看右上、sad 垂眼、
// cool 侧视、embarrassed 移开视线；正脸情绪保持居中。
static const MoodData kMoods[] = {
    {0, 0, 0, 30, 45, 0, 0},               // neutral
    {0, 50, 0, 30, 45, 0, -0.10f},         // happy
    {10, 65, 0, 25, 45, 0, -0.15f},        // laughing
    {5, 55, 0, 28, 45, 0.15f, 0},          // funny
    {0, 0, -32, 27, 45, 0, 0.15f},         // sad（柔和垂眼）
    {0, 0, 35, 30, 45, 0, 0},              // angry（可识别但不瞪视）
    {24, 0, -30, 24, 43, 0, 0.15f},        // crying
    {0, 45, 0, 32, 48, 0, -0.10f},         // loving
    {25, 15, 0, 22, 40, -0.25f, 0.25f},    // embarrassed（移开视线）
    {0, 0, 0, 38, 52, 0, 0},               // surprised（居中放大）
    {0, 0, -6, 38, 50, 0, 0},              // shocked
    {10, 15, -20, 28, 43, 0.45f, -0.40f},  // thinking（看右上）
    {0, 35, 25, 0, 45, 0.15f, 0},          // winking
    {15, 20, 10, 28, 45, 0.40f, 0},        // cool（侧视）
    {35, 0, 0, 25, 43, 0, 0.10f},          // relaxed
    {5, 60, 0, 28, 45, 0.20f, -0.20f},     // delicious
    {0, 50, 0, 18, 40, 0, -0.05f},         // kissy
    {10, 15, 15, 30, 45, 0.25f, -0.10f},   // confident
    {55, 0, 0, 20, 42, 0, 0},              // sleepy（半闭眼、视线居中）
    {0, 40, 0, 30, 45, -0.15f, 0},         // silly
    {5, 0, -30, 25, 43, -0.30f, 0},        // confused
};
// 全脸参数：左右眉角、眉高、嘴角、嘴张合、腮红、泪滴、汗滴、火花。
// 构图参考 M5Stack-Avatar，动画机制参考 LVGL Kawaii Face；仅采用公开视觉思想，
// 不复制第三方实现。正 mouthCurve=上扬，负值=下垂；效果量归一化为 0..1。
static const FacialData kFacialMoods[] = {
    {0, 0, 0, 0, 0.48f, 0.12f, 0, 0, 0, 0},                    // neutral
    {0, 0, 0.10f, 0.10f, 0.85f, 0.18f, 0.45f, 0, 0, 0},        // happy
    {-4, -4, 0.15f, 0.15f, 1.00f, 0.75f, 0.55f, 0, 0, 0.5f},   // laughing
    {-8, 10, 0.05f, 0.16f, 0.75f, 0.35f, 0.35f, 0, 0, 0.4f},   // funny
    {-8, -8, -0.03f, -0.03f, -0.35f, 0.05f, 0, 0, 0, 0},       // sad
    {18, 18, -0.04f, -0.04f, -0.20f, 0.06f, 0, 0, 0, 0},       // angry
    {-8, -8, -0.06f, -0.06f, -0.40f, 0.18f, 0, 1, 0, 0},       // crying
    {-5, -5, 0.12f, 0.12f, 0.90f, 0.12f, 0.80f, 0, 0, 0.8f},   // loving
    {-10, 12, 0, 0.05f, 0.20f, 0.05f, 0.85f, 0, 0.25f, 0},     // embarrassed
    {0, 0, 0.20f, 0.20f, 0.05f, 0.65f, 0, 0, 0, 0},            // surprised
    {0, 0, 0.18f, 0.18f, 0, 0.72f, 0, 0, 0.40f, 0},            // shocked
    {-18, 22, 0.12f, -0.02f, -0.10f, 0.04f, 0, 0, 0, 0},       // thinking
    {-8, 12, 0.10f, 0.05f, 0.72f, 0.10f, 0.30f, 0, 0, 0.3f},   // winking
    {-4, 8, -0.02f, 0.06f, 0.22f, 0.04f, 0, 0, 0, 0.2f},       // cool
    {0, 0, -0.08f, -0.08f, 0.48f, 0.05f, 0.15f, 0, 0, 0},      // relaxed
    {-8, 10, 0.05f, 0.10f, 0.88f, 0.25f, 0.55f, 0, 0, 0.4f},   // delicious
    {-6, -6, 0.08f, 0.08f, 0.65f, 0.02f, 0.75f, 0, 0, 0.5f},   // kissy
    {8, 8, 0.06f, 0.06f, 0.38f, 0.04f, 0, 0, 0, 0},            // confident
    {0, 0, 0.22f, 0.02f, 0.45f, 0.02f, 0, 0, 0, 0},            // sleepy
    {18, -6, 0.14f, 0.03f, 0.80f, 0.40f, 0.30f, 0, 0, 0.25f},  // silly
    {20, -18, 0.12f, -0.04f, -0.22f, 0.06f, 0, 0, 0.2f, 0},    // confused
};
// 情绪配色：整张脸铺与开机 π logo 静止帧完全相同的 0..1 全程对角线渐变
// （左下热粉→右上薄荷）。本表的 t 不再是取色点，而是情绪相位：相对品牌中段
// 的偏移经 kPiFacePhaseSwing 压缩成 ±0.15 的彩虹旋转（见 RecomputePalette），
// 情绪保持可辨，但任何情绪下脸都和 π 一样是彩虹。语义映射：暖端(粉/品红) =
// 爱/馋/怒等高唤醒，中段(紫/长春花) = 思考/窘/困/悲，冷端(青/薄荷) =
// 平静/酷/喜悦族。勿手写十六进制——那会脱离渐变、破坏和谐。
static constexpr float kMoodGradientT[] = {
    kPiBrandGradientT,  // neutral：π 主视觉中段长春花蓝紫
    0.99f,              // happy：薄荷（喜悦族冷端）
    1.00f,              // laughing：薄荷末端，最亮
    0.94f,              // funny：薄荷偏青
    0.62f,              // sad：长春花偏青，区别于 neutral 品牌蓝紫
    0.02f,              // angry：热粉起点，最暖最强
    0.70f,              // crying：青蓝，区别于 sad 与 shocked
    0.06f,              // loving：粉紫
    0.22f,              // embarrassed：紫罗兰
    0.76f,              // surprised：亮青
    0.66f,              // shocked：青蓝，比 surprised 冷一档
    0.32f,              // thinking：紫
    0.90f,              // winking：青绿（喜悦族）
    0.80f,              // cool：亮青
    0.96f,              // relaxed：薄荷
    0.10f,              // delicious：品红（暖端，馋）
    0.04f,              // kissy：热粉偏紫
    0.84f,              // confident：亮青
    0.44f,              // sleepy：长春花偏紫
    0.16f,              // silly：紫粉
    0.36f,              // confused：紫偏长春花
};
// 面部点缀色同样取 π 渐变，不再手写十六进制：舌/腮红取暖端粉，泪取中段长春花，
// 汗取冷端青，星芒取薄荷末端（最亮）。原值 0xFF7EB6/0x7FB3FF/0xBDF3FF/0xFFD60A
// 里的暖黄星芒完全落在渐变外，是开机后最扎眼的不和谐点。
static constexpr float kTongueGradientT = 0.03f;
static constexpr float kBlushGradientT = 0.03f;
static constexpr float kTearGradientT = 0.55f;
static constexpr float kSweatGradientT = 0.75f;
static constexpr float kSparkleGradientT = 0.99f;
static const char* kNames[] = {
    "neutral", "happy",       "laughing",  "funny",     "sad",      "angry",   "crying",
    "loving",  "embarrassed", "surprised", "shocked",   "thinking", "winking", "cool",
    "relaxed", "delicious",   "kissy",     "confident", "sleepy",   "silly",   "confused",
};
static_assert(std::size(kMoods) == std::size(kNames));
static_assert(std::size(kFacialMoods) == std::size(kNames));
static_assert(std::size(kMoodGradientT) == std::size(kNames));
static constexpr size_t kMoodCount = std::size(kNames);
static constexpr int kSleepyMoodIndex = 18;

GrobotEyes::GrobotEyes(lv_color_t bgColor) : bg_color_(bgColor) {
    RecomputePalette(kMoodGradientT[0]);
    curL_ = {0, 0, 0, kPupilRadius, kEyeRadius, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    curR_ = targetL_ = targetR_ = baseL_ = baseR_ = curL_;
    face_cur_ = face_target_ = kFacialMoods[0];
    last_frame_us_ = last_blink_us_ = esp_timer_get_time();
}
void GrobotEyes::RecomputePalette(float mood_t) {
    // 相位 = 情绪 t 相对品牌中段的偏移 × kPiFacePhaseSwing（±0.15）：
    // 情绪让整条彩虹轻微旋转，而不是换成另一个平色。
    mood_base_phase_ = (mood_t - kPiBrandGradientT) * kPiFacePhaseSwing;
    BuildShadeLut(mood_base_phase_, 0.0f, -1.0f);
}

void GrobotEyes::BuildShadeLut(float phase, float shine_strength, float shine_pos) {
    // LUT 第 i 级 = 对角线 base=i/(steps-1) 叠加相位后的渐变取色，
    // 再经上游 shine 合成白色高光——与 logo 动画同一公式。
    // 相位映射用钳制而非绕回：绕回会在脸内留下薄荷|热粉硬边；钳制让潮汐
    // 在两角形成连续的粉/薄荷「淤积」高原——暖情绪粉淤左下、冷情绪薄荷淤
    // 右上，反而强化情绪语义，且全程无接缝。
    const lv_color_t center_eye = lv_color_hex(PiGradientHex(PiWrap01(0.5f + phase)));
    scan_color_ = lv_color_mix(center_eye, bg_color_, 7);
    for (int i = 0; i < kFaceShadeSteps; i++) {
        const float base = (float)i / (float)(kFaceShadeSteps - 1);
        const float tc = std::clamp(base + phase, 0.0f, 1.0f);
        uint8_t pr = 0, pg = 0, pb = 0;
        PiGradientRgb(tc, shine_strength, shine_pos, &pr, &pg, &pb);
        const lv_color_t eye = lv_color_make(pr, pg, pb);
        const lv_color_t white = lv_color_white();
        shade_lut_[kShadeGlow0][i] = lv_color_to_u16(lv_color_mix(eye, bg_color_, 38));
        shade_lut_[kShadeGlow1][i] = lv_color_to_u16(lv_color_mix(eye, bg_color_, 77));
        shade_lut_[kShadeGlow2][i] = lv_color_to_u16(lv_color_mix(eye, bg_color_, 128));
        shade_lut_[kShadeIris0][i] = lv_color_to_u16(lv_color_mix(eye, bg_color_, 153));
        shade_lut_[kShadeIris1][i] = lv_color_to_u16(eye);
        shade_lut_[kShadeIris2][i] = lv_color_to_u16(lv_color_mix(white, eye, 77));
        shade_lut_[kShadePupil][i] = lv_color_to_u16(lv_color_mix(eye, bg_color_, 25));
        shade_lut_[kShadeHighlight][i] = lv_color_to_u16(lv_color_mix(white, eye, 180));
        shade_lut_[kShadeEye][i] = lv_color_to_u16(eye);
        shade_lut_[kShadeBrow][i] = lv_color_to_u16(lv_color_mix(eye, white, 55));
        shade_lut_[kShadeNose][i] = lv_color_to_u16(lv_color_mix(eye, bg_color_, 145));
        shade_lut_[kShadeNoseHi][i] = lv_color_to_u16(lv_color_mix(white, eye, 45));
        shade_lut_[kShadeNoseShadow][i] = lv_color_to_u16(lv_color_mix(bg_color_, eye, 25));
        const lv_color_t mouth = lv_color_mix(eye, white, 45);
        shade_lut_[kShadeMouth][i] = lv_color_to_u16(mouth);
        shade_lut_[kShadeMouthCorner][i] = lv_color_to_u16(lv_color_mix(mouth, bg_color_, 48));
    }
}

GrobotEyes::~GrobotEyes() {
    if (timer_)
        lv_timer_delete(timer_);
    if (canvas_)
        lv_obj_delete(canvas_);
    if (draw_buf_)
        lv_draw_buf_destroy(draw_buf_);
}

bool GrobotEyes::Init(lv_obj_t* parent, int w, int h) {
    w_ = w;
    h_ = h;
    // 对角线 LUT 索引的定点标度：idx = (x + (h-1-y)) * scale >> 16。
    // 分母 w+h-1 与 PiFaceGradientT 的 span 同源（+1 技巧，base 严格 < 1）。
    shade_scale_q16_ = (int32_t)(((int64_t)(kFaceShadeSteps - 1) << 16) / (w_ + h_ - 1));
    layout_.scale = (float)h_ / kBaseH;
    layout_.centerY = h_ / 2;
    layout_.eyeRadius = (int)lroundf(kEyeRadius * layout_.scale);
    layout_.eyeOffset = (int)lroundf((kEyeRadius + kEyeGap) * layout_.scale);
    if (w_ >= 400 && h_ >= 280) {
        constexpr float kPhi = 1.61803398875f;
        layout_.scale *= 1.08f;
        layout_.eyeRadius = (int)lroundf(kEyeRadius * layout_.scale);
        layout_.eyeOffset = (int)lroundf(layout_.eyeRadius * kPhi * 0.90f);
        const int eyeR = layout_.eyeRadius;
        layout_.centerY = h_ / 2 + eyeR / 5;
    }
    draw_buf_ = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
    if (draw_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate GrobotEyes draw buffer (%dx%d)", w, h);
        return false;
    }
    canvas_ = lv_canvas_create(parent);
    if (canvas_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create GrobotEyes canvas");
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
        return false;
    }
    // Canvas 继承 lv_obj 的默认滚动/链标志；触摸脸时必须在源头截断滚动手势。
    lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_canvas_set_draw_buf(canvas_, draw_buf_);
    lv_obj_center(canvas_);
    buf_ = (uint16_t*)draw_buf_->data;
    timer_ = lv_timer_create(TimerCb, 33, this);
    if (timer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create GrobotEyes timer");
        lv_obj_delete(canvas_);
        canvas_ = nullptr;
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
        buf_ = nullptr;
        return false;
    }
    return true;
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
    if (mood_index_ == kSleepyMoodIndex) {
        targetL_.topH = baseL_.topH;
        targetR_.topH = baseR_.topH;
        return;
    }
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
    if (emotion == nullptr) {
        emotion = "neutral";
    }
    MoodData left = kMoods[0], right = kMoods[0];
    // 未命中的情绪回落 neutral 的渐变位置，而不是主题强调色——后者不在 π 渐变上，
    // 会在开机交接后跳色。
    float mood_t = kMoodGradientT[0];
    mood_index_ = -1;
    for (int i = 0; i < kMoodCount; i++) {
        if (strcmp(emotion, kNames[i]) == 0) {
            mood_index_ = i;
            left = right = kMoods[i];
            mood_t = kMoodGradientT[i];
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
    RecomputePalette(mood_t);
    last_blink_us_ = esp_timer_get_time();
}

void GrobotEyes::SetSpeaking(bool on) { speaking_ = on; }

void GrobotEyes::SetListening(bool on) { listening_ = on; }

void GrobotEyes::SetPaused(bool on) {
    if (timer_ == nullptr) {
        return;
    }
    if (on) {
        lv_timer_pause(timer_);
    } else {
        lv_timer_resume(timer_);
    }
}

void GrobotEyes::BufHLine(int x0, int x1, int y, FacePaint p) {
    if (y < 0 || y >= h_)
        return;
    int s = draw_buf_->header.stride / 2;
    for (int x = std::max(0, x0); x <= std::min(w_ - 1, x1); x++)
        buf_[y * s + x] = Resolve(p, x, y);
}

void GrobotEyes::BufFillRect(int x, int y, int w, int h, FacePaint p) {
    int s = draw_buf_->header.stride / 2;
    for (int r = std::max(0, y); r < std::min(h_, y + h); r++)
        for (int col = std::max(0, x); col < std::min(w_, x + w); col++)
            buf_[r * s + col] = Resolve(p, col, r);
}

uint16_t GrobotEyes::Blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
    const uint32_t r1 = (fg >> 11) & 0x1F, g1 = (fg >> 5) & 0x3F, b1 = fg & 0x1F;
    const uint32_t r2 = (bg >> 11) & 0x1F, g2 = (bg >> 5) & 0x3F, b2 = bg & 0x1F;
    const uint32_t r = (r1 * alpha + r2 * (255 - alpha)) / 255;
    const uint32_t g = (g1 * alpha + g2 * (255 - alpha)) / 255;
    const uint32_t b = (b1 * alpha + b2 * (255 - alpha)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void GrobotEyes::BufFillCircle(int cx, int cy, int r, FacePaint p) {
    if (r <= 0)
        return;
    const int s = draw_buf_->header.stride / 2;
    const float rf = (float)r;
    for (int row = std::max(0, cy - r); row <= std::min(h_ - 1, cy + r); row++) {
        const float dy = (float)(row - cy);
        const float dx_f = sqrtf(rf * rf - dy * dy);
        const int dx = (int)dx_f;
        for (int col = std::max(0, cx - dx); col <= std::min(w_ - 1, cx + dx); col++)
            buf_[row * s + col] = Resolve(p, col, row);
        // 边缘 1px 抗锯齿：按覆盖率分数与底层像素混合（不是背景色——圆会叠在
        // 辉光环/眼白上，与底色混会出现脏边）。渐变时两端各自取本地色。
        const uint8_t frac = (uint8_t)((dx_f - dx) * 255.0f);
        if (frac > 0) {
            const int cl = cx - dx - 1, cr = cx + dx + 1;
            if (cl >= 0)
                buf_[row * s + cl] = Blend565(Resolve(p, cl, row), buf_[row * s + cl], frac);
            if (cr < w_)
                buf_[row * s + cr] = Blend565(Resolve(p, cr, row), buf_[row * s + cr], frac);
        }
    }
}
void GrobotEyes::BufFillEllipse(int cx, int cy, int rx, int ry, FacePaint p) {
    if (rx <= 0 || ry <= 0)
        return;
    for (int y = std::max(0, cy - ry); y <= std::min(h_ - 1, cy + ry); y++) {
        const float ny = (float)(y - cy) / ry;
        const int dx = (int)(rx * sqrtf(std::max(0.0f, 1.0f - ny * ny)));
        BufHLine(cx - dx, cx + dx, y, p);
    }
}

void GrobotEyes::BufLine(int x0, int y0, int x1, int y1, FacePaint p, int thickness) {
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        BufFillCircle(x0, y0, std::max(1, thickness), p);
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
    const int yL = cy - eyeR - eyeR / 2 - (int)(face_cur_.browLiftL * eyeR * 2 / 5);
    const int yR = cy - eyeR - eyeR / 2 - (int)(face_cur_.browLiftR * eyeR * 2 / 5);
    const int halfW = eyeR * 3 / 4;
    const int tiltL = (int)(face_cur_.browTiltL * 0.35f);
    const int tiltR = (int)(face_cur_.browTiltR * 0.35f);
    const int browArch = std::max(10, eyeR / 6);
    const int browBaseOffset = eyeR / 10;
    const int browWeight = std::max(2, eyeR / 24);
    const int browGlowWeight = browWeight + 1;
    auto drawBrow = [&](int browCx, int browY, int tilt) {
        browY += browBaseOffset;
        int prevX = browCx - halfW;
        int prevY = browY - tilt;
        for (int i = 1; i <= 12; i++) {
            const float nx = -1.0f + 2.0f * i / 12.0f;
            const int x = browCx - halfW + 2 * halfW * i / 12;
            const int slope = -tilt + 2 * tilt * i / 12;
            const int y = browY + slope - (int)(browArch * (1.0f - nx * nx));
            BufLine(prevX, prevY + 2, x, y + 2, Shade(kShadeGlow0), browGlowWeight);
            BufLine(prevX, prevY, x, y, Shade(kShadeBrow), browWeight);
            prevX = x;
            prevY = y;
        }
    };
    drawBrow(cx - offset, yL, tiltL);
    drawBrow(cx + offset, yR, -tiltR);
}

void GrobotEyes::DrawNose(int cx, int cy, int eyeR) {
    const int noseY = cy + eyeR / 2 + std::max(4, eyeR / 12);
    const int noseRx = std::max(14, eyeR / 3);
    const int noseRy = std::max(7, eyeR / 8);
    // 实体圆角机器人鼻：比旧挖空短划更像完整五官，但仍弱于眼睛和嘴。
    BufFillEllipse(cx, noseY, noseRx, noseRy, Shade(kShadeGlow1));
    BufFillEllipse(cx, noseY + 3, noseRx - 1, noseRy, Shade(kShadeNoseShadow));
    BufFillEllipse(cx, noseY, noseRx - 2, noseRy - 2, Shade(kShadeNose));
    BufLine(cx - noseRx / 2, noseY - noseRy / 2, cx + noseRx / 3, noseY - noseRy / 2,
            Shade(kShadeNoseHi), 1);
}

void GrobotEyes::DrawMouth(int cx, int cy, int eyeR) {
    const int width = eyeR * 11 / 5;
    const int curve = (int)(face_cur_.mouthCurve * eyeR * 2 / 5);
    const int open = std::max(3, (int)(face_cur_.mouthOpen * eyeR / 2));
    const int lipGap = std::max(6, open / 2);
    const int mouthY = cy + eyeR + eyeR * 11 / 40;
    const int upperLipWeight = std::max(2, eyeR / 20);
    const int lowerLipWeight = std::max(1, eyeR / 28);
    if (open > 5) {
        const lv_color_t tongue = lv_color_hex(PiGradientHex(kTongueGradientT));
        BufFillEllipse(cx, mouthY + curve / 4, width / 2, open, Shade(kShadeMouth));
        BufFillEllipse(cx, mouthY + curve / 4, width / 2 - 4, std::max(3, open - 4), bg_color_);
        BufFillEllipse(cx, mouthY + open / 2 + curve / 4, width / 3, std::max(3, open / 3), tongue);
        return;
    }
    int prevX = cx - width / 2;
    int prevUpperY = mouthY;
    int prevLowerY = mouthY;
    for (int i = 1; i <= 16; i++) {
        const float nx = -1.0f + (2.0f * i / 16.0f);
        const int x = cx - width / 2 + width * i / 16;
        const int lipArc = (int)(curve * (1.0f - nx * nx));
        const int iabs = std::abs(i - 8);
        const int lipSeparation = lipGap - lipGap * iabs / 8;
        const int nextUpperY = mouthY + lipArc - lipSeparation / 2;
        const int nextLowerY = mouthY + lipArc + lipSeparation / 2;
        BufLine(prevX, prevUpperY, x, nextUpperY, Shade(kShadeMouth), upperLipWeight);
        BufLine(prevX, prevLowerY, x, nextLowerY, Shade(kShadeGlow2), lowerLipWeight);
        prevX = x;
        prevUpperY = nextUpperY;
        prevLowerY = nextLowerY;
    }
    BufFillCircle(cx - width / 2, mouthY, upperLipWeight + 1, Shade(kShadeMouthCorner));
    BufFillCircle(cx + width / 2, mouthY, upperLipWeight + 1, Shade(kShadeMouthCorner));
}
void GrobotEyes::DrawFacialEffects(int cx, int cy, int eyeR, int offset) {
    const int blush = (int)(face_cur_.blush * 18);
    if (blush > 1) {
        const lv_color_t blushColor = lv_color_hex(PiGradientHex(kBlushGradientT));
        BufFillEllipse(cx - offset - eyeR, cy + eyeR / 2, blush, std::max(2, blush / 3),
                       blushColor);
        BufFillEllipse(cx + offset + eyeR, cy + eyeR / 2, blush, std::max(2, blush / 3),
                       blushColor);
    }
    const int tears = (int)(face_cur_.tears * 24);
    if (tears > 2) {
        const lv_color_t tearColor = lv_color_hex(PiGradientHex(kTearGradientT));
        BufLine(cx - offset, cy + eyeR / 2, cx - offset - 3, cy + eyeR / 2 + tears, tearColor, 2);
        BufLine(cx + offset, cy + eyeR / 2, cx + offset + 3, cy + eyeR / 2 + tears, tearColor, 2);
        BufFillCircle(cx - offset - 3, cy + eyeR / 2 + tears, 4, tearColor);
        BufFillCircle(cx + offset + 3, cy + eyeR / 2 + tears, 4, tearColor);
    }
    if (face_cur_.sweat > 0.08f) {
        const int drop = 7 + (int)(face_cur_.sweat * 10);
        const lv_color_t sweatColor = lv_color_hex(PiGradientHex(kSweatGradientT));
        BufFillCircle(cx + offset + eyeR + 12, cy - eyeR / 2, drop / 2, sweatColor);
        BufFillTriangle(cx + offset + eyeR + 12 - drop / 2, cy - eyeR / 2,
                        cx + offset + eyeR + 12 + drop / 2, cy - eyeR / 2, cx + offset + eyeR + 12,
                        cy - eyeR / 2 + drop, sweatColor);
    }
    if (face_cur_.sparkle > 0.08f) {
        const int r = 5 + (int)(face_cur_.sparkle * 7);
        const int sx = cx + offset + eyeR + 17, sy = cy - eyeR;
        const lv_color_t sparkleColor = lv_color_hex(PiGradientHex(kSparkleGradientT));
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

void GrobotEyes::BufFillHeart(int cx, int cy, int r, FacePaint p) {
    // 两圆一三角拼心形；r 是整体半宽。尺寸小（~12px），比例宁宽勿瘦才认得出。
    const int hr = std::max(2, r / 2);
    BufFillCircle(cx - hr, cy - hr / 2, hr, p);
    BufFillCircle(cx + hr, cy - hr / 2, hr, p);
    BufFillTriangle(cx - 2 * hr, cy - hr / 2, cx + 2 * hr, cy - hr / 2, cx, cy + r, p);
}

void GrobotEyes::BufFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, FacePaint p) {
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
        BufHLine(std::min(xa, xb), std::max(xa, xb), y, p);
    }
}

void GrobotEyes::DrawBackground() {
    lv_canvas_fill_bg(canvas_, bg_color_, LV_OPA_COVER);
    buf_ = (uint16_t*)draw_buf_->data;
    for (int y = 5; y < h_; y += 6)
        BufHLine(0, w_ - 1, y, scan_color_);
}

void GrobotEyes::DrawEye(int cx, int cy, int gazeX, int gazeY, int pR, int eyeR, int lidH, int botH,
                         int tilt, bool isLeft, bool heartPupil) {
    float phase = (float)(last_frame_us_ % 10000000) / 10000000.0f * 6.2832f;
    int pulse = mood_index_ == kSleepyMoodIndex ? 0 : (int)(1.0f * sinf(phase));
    BufFillCircle(cx, cy, eyeR + 10 + pulse, Shade(kShadeGlow0));
    BufFillCircle(cx, cy, eyeR + 9 + pulse, bg_color_);
    BufFillCircle(cx, cy, eyeR + 7, Shade(kShadeGlow0));
    BufFillCircle(cx, cy, eyeR + 4, Shade(kShadeGlow1));
    BufFillCircle(cx, cy, eyeR + 2, Shade(kShadeGlow2));
    BufFillCircle(cx, cy, eyeR, Shade(kShadeEye));
    // 眼球（虹膜+瞳+高光）随视线偏移；辉光环与眼睑遮罩保持原位，视线偏移上界
    // 在 Render 侧按 eyeR 比例限制，虹膜不会跑出眼白。
    int pcx = cx + gazeX, pcy = cy + gazeY;
    int ir = eyeR * 3 / 4;
    BufFillCircle(pcx, pcy, ir, Shade(kShadeIris0));
    BufFillCircle(pcx, pcy, ir * 3 / 4, Shade(kShadeIris1));
    BufFillCircle(pcx, pcy, ir / 2, Shade(kShadeIris2));
    if (heartPupil)
        BufFillHeart(pcx, pcy, std::max(5, pR * 2 / 5 + 1), Shade(kShadePupil));
    else
        BufFillCircle(pcx, pcy, std::max(3, pR * 2 / 5), Shade(kShadePupil));
    BufFillCircle(pcx - eyeR / 4, pcy - eyeR / 4, std::max(2, eyeR / 8), Shade(kShadeHighlight));
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
    // 「活」的两层（都与 logo 同源）：
    // 1) 色相潮汐：基底相位 5s 正弦 ±0.22 持续往复，整脸彩虹永不静止；
    //    相位经钳制映射，极端相位只在两角形成渐变高原，无接缝。
    // 2) 高光扫脸：上游 shine 公式。说话 1.5s/周连续强扫（0.90），
    //    聆听 3s/周连续扫（0.90，介于说话与空闲之间）——配合瞳孔放大 18%，
    //    对话距离一眼可辨「已唤醒、在听」（2026-08-28 用户主诉：唤醒无感知）；
    //    空闲每 6s 扫 2s（0.85，接近上游峰值的亮带），sleepy 全停。
    float tide = 0.0f, shine_strength = 0.0f, shine_pos = -1.0f;
    if (mood_index_ != kSleepyMoodIndex) {
        tide = 0.22f * sinf((float)(last_frame_us_ % 5000000) / 5000000.0f * 6.2832f);
        const int64_t period_us = speaking_ ? 1500000 : (listening_ ? 3000000 : 6000000);
        const float duty = (speaking_ || listening_) ? 1.0f : 0.34f;
        const float t01 = (float)(last_frame_us_ % period_us) / (float)period_us;
        if (t01 < duty) {
            // 从 -半宽扫到 1+半宽，进出都无残影。
            shine_pos = t01 / duty * (1.0f + 2.0f * kPiShineHalfWidth) - kPiShineHalfWidth;
            shine_strength = (speaking_ || listening_) ? 0.90f : 0.85f;
        }
    }
    BuildShadeLut(mood_base_phase_ + tide, shine_strength, shine_pos);
    DrawBackground();
    Blink();
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
    auto s = [&](float v) { return (int)lroundf(v * layout_.scale); };
    const int offset = layout_.eyeOffset;
    const int cx = w_ / 2;
    const int cy = layout_.centerY;
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
    if (!speaking_ && !listening_ && mood_index_ != kSleepyMoodIndex) {
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
    // 说话弹跳约 6Hz ±1px；保留活动反馈，同时限制未成年人界面的持续视觉刺激。
    // 聆听时瞳孔放大 18%。
    int bounce = 0;
    if (speaking_) {
        bounce = (int)(1.0f * sinf((float)(last_frame_us_ % 166667) / 166667.0f * 6.2832f));
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
