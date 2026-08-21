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

struct FacialData {
    float browTiltL;
    float browTiltR;
    float browLiftL;
    float browLiftR;
    float mouthCurve;
    float mouthOpen;
    float blush;
    float tears;
    float sweat;
    float sparkle;
};

struct EyeState {
    float topH, botH, tilt, pR, eyeRadius;
    float lookX, lookY;
    float vTop, vBot, vTilt, vPR, vRadius, vLookX, vLookY;
};

struct FaceLayout {
    float scale;
    int centerY;
    int eyeRadius;
    int eyeOffset;
};

// 渐变着色的角色表：每个角色一行 64 级 LUT，存放沿对角线从暖到冷的 RGB565。
// 权重复刻原平色时代的 lv_color_mix 配方，保证渐变中段的观感与旧平色一致。
enum FaceShadeRole : uint8_t {
    kShadeScan = 0,
    kShadeGlow0,
    kShadeGlow1,
    kShadeGlow2,
    kShadeIris0,
    kShadeIris1,
    kShadeIris2,
    kShadePupil,
    kShadeHighlight,
    kShadeEye,
    kShadeBrow,
    kShadeNose,
    kShadeNoseHi,
    kShadeNoseShadow,
    kShadeMouth,
    kShadeMouthCorner,
    kShadeRoleCount
};

/** 图元着色源：要么平色（lv_color_t 隐式转换），要么沿脸对角线查渐变 LUT。
 *  调用点传 lv_color_t 时代码不变；要渐变时传 Shade(role)。 */
struct FacePaint {
    const uint16_t* lut;  // 非空：按像素对角线位置查表
    uint16_t flat;        // lut 为空时的平色（RGB565）
    FacePaint(lv_color_t c) : lut(nullptr), flat(lv_color_to_u16(c)) {}
    explicit FacePaint(const uint16_t* lut_row) : lut(lut_row), flat(0) {}
};

class GrobotEyes {
public:
    /** 眼色不再由调用方传入：整张脸铺与开机 π logo 同一条对角线渐变
     * （kMoodGradientT 定窗口中心，kPiFaceGradientWindow 定宽度）。背景色仍随主题。 */
    explicit GrobotEyes(lv_color_t bgColor);
    ~GrobotEyes();
    bool Init(lv_obj_t* parent, int w, int h);
    void SetEmotion(const char* emotion);
    /** 说话中：嘴巴随节律开合，整张脸轻微弹跳。 */
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
    void ApplyFacialData(const FacialData& data);
    void DrawEyebrows(int cx, int cy, int eyeR, int offset);
    void DrawNose(int cx, int cy, int eyeR);
    void DrawMouth(int cx, int cy, int eyeR);
    void DrawFacialEffects(int cx, int cy, int eyeR, int offset);
    void BufFillEllipse(int cx, int cy, int rx, int ry, FacePaint p);
    void BufLine(int x0, int y0, int x1, int y1, FacePaint p, int thickness = 1);
    void DrawBackground();
    void BufFillRect(int x, int y, int w, int h, FacePaint p);
    void BufFillCircle(int cx, int cy, int r, FacePaint p);
    void BufFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, FacePaint p);
    void BufHLine(int x0, int x1, int y, FacePaint p);
    /** 弧线眼睑：逐列抛物线切割（中央最高、边缘收 45%），替代直边矩形。 */
    void BufLidArc(int cx, int cy, int eyeR, int pad, int lidH, bool top);
    /** 心形瞳：两圆一三角拼装，loving/kissy 专用。 */
    void BufFillHeart(int cx, int cy, int r, FacePaint p);
    /** RGB565 空间按 alpha 混合（圆边抗锯齿用，与底层像素混合而非背景色）。 */
    static uint16_t Blend565(uint16_t fg, uint16_t bg, uint8_t alpha);

    void RecomputePalette(float mood_t);
    /** 只重建 64 级 LUT：基底相位 + 高光带位置/强度。潮汐/扫光期间每帧调用。 */
    void BuildShadeLut(float phase, float shine_strength, float shine_pos);

    FacePaint Shade(FaceShadeRole role) const { return FacePaint(shade_lut_[role]); }
    /** 平色直通；渐变角色按像素的对角线位置查 64 级 LUT（一次乘加，Q16 定点）。 */
    inline uint16_t Resolve(const FacePaint& p, int x, int y) const {
        if (p.lut == nullptr) {
            return p.flat;
        }
        const int32_t idx = ((int32_t)(x + h_ - 1 - y) * shade_scale_q16_) >> 16;
        return p.lut[idx < 0 ? 0 : (idx >= kFaceShadeSteps ? kFaceShadeSteps - 1 : idx)];
    }

    lv_obj_t* canvas_ = nullptr;
    lv_draw_buf_t* draw_buf_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint16_t* buf_ = nullptr;
    int w_ = 0, h_ = 0;

    lv_color_t bg_color_;
    lv_color_t scan_color_;
    // 渐变 LUT：kShadeRoleCount 行 × kFaceShadeSteps 列，仅情绪切换时重建。
    static constexpr int kFaceShadeSteps = 64;
    uint16_t shade_lut_[kShadeRoleCount][kFaceShadeSteps];
    int32_t shade_scale_q16_ = 0;  // (steps-1)/(w+h-1)，Q16
    float mood_base_phase_ = 0;    // 当前情绪的基底相位（±0.15 内），潮汐在其上叠加
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
    FacialData face_cur_{}, face_target_{}, face_vel_{};

    FaceLayout layout_{};
    int64_t last_frame_us_ = 0;
    int64_t last_blink_us_ = 0;
    int blink_interval_ms_ = 3000;
    int blink_dur_ms_ = 250;
    float dt_ = 0;

    static constexpr float kEyeRadius = 45.0f;
    static constexpr float kPupilRadius = 30.0f;
    static constexpr int kEyeGap = 20;
    static constexpr float kBaseH = 190.0f;
};

#endif
