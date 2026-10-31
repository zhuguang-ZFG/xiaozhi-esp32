#ifndef HUTUJI_PI_SPLASH_CORE_H
#define HUTUJI_PI_SPLASH_CORE_H

// omp 启动 logo 的可移植核心：字形、配色、动画参数全部逐字复刻上游
// can1357/oh-my-pi `packages/coding-agent/src/modes/components/welcome.ts`
// （2026-08-21 取自 raw.githubusercontent.com main 分支）。
//
// 本头文件不依赖 LVGL / ESP-IDF，可在 host 上编译测试；
// `tmp/pi_logo_preview.py` 直接解析本文件的 kPiLogoRows 与色标，
// 保证预览图与板上渲染同源，不会两边漂移。
//
// 上游字形是 5 行 × 12 列 Unicode 方块字符：
//   "▀██████████▀"
//   " ╘██    ██  "
//   "  ██    ██  "
//   "  ██    ██  "
//   " ▄██▄  ▄██▄ "
// 这里改用 ASCII 记号编码，避免源码编码依赖，映射固定为：
//   'F' = █ 全块, 'U' = ▀ 上半块, 'L' = ▄ 下半块, 'C' = ╘, ' ' = 空。

#include <cmath>
#include <cstdint>

// PI_LOGO（上游同名常量）。行列数是渐变对角线归一化的分母来源，勿改。
static const char* const kPiLogoRows[] = {
    "UFFFFFFFFFFU", " CFF    FF  ", "  FF    FF  ", "  FF    FF  ", " LFFL  LFFL ",
};
static constexpr int kPiLogoRowCount = 5;
static constexpr int kPiLogoColCount = 12;
// span：上游 `Math.max(1, cols + rows - 1)`，+1 效应让 base 严格 < 1，
// 避免远角回绕到 t=0（热粉）破坏静止帧。
static constexpr int kPiLogoSpan = kPiLogoColCount + kPiLogoRowCount - 1;

// GRADIENT_STOPS（上游同名常量）：热粉 → 紫罗兰 → 长春花 → 亮青 → 薄荷。
static constexpr int kPiGradientStopCount = 5;
static constexpr uint8_t kPiGradientStops[kPiGradientStopCount][3] = {
    {255, 92, 200},   // hot pink
    {200, 110, 255},  // violet
    {120, 130, 255},  // periwinkle
    {60, 200, 255},   // bright cyan
    {120, 255, 220},  // mint
};

// π 视觉体系在常驻 UI 的语义位置。这些不是上游动画参数，而是本机从同一条上游
// 渐变抽出的角色：Grobot 默认眼色和主按钮共用中段蓝紫；成功/警告/危险依次取
// 薄荷、紫罗兰、热粉。角色只存 t，不手抄 RGB，渐变色标若变会整体同步。
static constexpr float kPiBrandGradientT = 0.50f;
static constexpr float kPiSuccessGradientT = 0.96f;
static constexpr float kPiWarningGradientT = 0.22f;
static constexpr float kPiDangerGradientT = 0.02f;

// SHINE_HALF_WIDTH：高光带半宽，单位是渐变 t 空间。
static constexpr float kPiShineHalfWidth = 0.18f;
// INTRO_MS / INTRO_TICK_MS / INTRO_SWEEPS / INTRO_SHINE_TRAVERSALS：上游原值。
// 3000ms 是「一比一复刻」的一部分，不得为了开机快而缩短。
static constexpr int kPiIntroMs = 3000;
static constexpr int kPiIntroTickMs = 33;
static constexpr float kPiIntroSweeps = 2.5f;
static constexpr float kPiIntroShineTraversals = 3.0f;

// 单元内子矩形（归一化 0..1）。终端一个字符格对应屏上一个单元；
// 方块字符按 Unicode 定义占满/占上半/占下半，'C' 的线宽取终端惯例 1/8 单元。
struct PiCellRect {
    float x0, y0, x1, y1;
};
static constexpr int kPiCellRectMax = 3;

/** 字符记号 → 子矩形列表；返回写入 out 的矩形数（0 表示空白）。 */
inline int PiCellRects(char code, PiCellRect out[kPiCellRectMax]) {
    switch (code) {
        case 'F':
            out[0] = {0.0f, 0.0f, 1.0f, 1.0f};
            return 1;
        case 'U':
            out[0] = {0.0f, 0.0f, 1.0f, 0.5f};
            return 1;
        case 'L':
            out[0] = {0.0f, 0.5f, 1.0f, 1.0f};
            return 1;
        case 'C': {
            // ╘ = box drawings up single and right double：竖线自中心向上，
            // 双横线自中心向右。线宽 1/8 单元，双线上下笔各 1/8、间隔 1/8。
            constexpr float vw = 0.125f;
            constexpr float hh = 0.125f;
            out[0] = {0.5f - vw / 2, 0.0f, 0.5f + vw / 2, 0.5f + hh * 1.5f};
            out[1] = {0.5f - vw / 2, 0.5f - hh * 1.5f, 1.0f, 0.5f - hh / 2};
            out[2] = {0.5f - vw / 2, 0.5f + hh / 2, 1.0f, 0.5f + hh * 1.5f};
            return 3;
        }
        default:
            return 0;
    }
}

/** 上游 gradientEscape() 的真彩分支：5 色标线性插值 + 白色高光合成。 */
inline void PiGradientRgb(float t, float shine_strength, float shine_pos, uint8_t* r, uint8_t* g,
                          uint8_t* b) {
    const float seg = t * (kPiGradientStopCount - 1);
    int i = (int)std::floor(seg);
    if (i > kPiGradientStopCount - 2)
        i = kPiGradientStopCount - 2;
    if (i < 0)
        i = 0;
    const float f = seg - (float)i;
    const uint8_t* a = kPiGradientStops[i];
    const uint8_t* c = kPiGradientStops[i + 1];
    float rf = (float)a[0] + ((float)c[0] - (float)a[0]) * f;
    float gf = (float)a[1] + ((float)c[1] - (float)a[1]) * f;
    float bf = (float)a[2] + ((float)c[2] - (float)a[2]) * f;
    if (shine_strength > 0.0f) {
        const float dist = std::fabs(t - shine_pos);
        float intensity = (1.0f - dist / kPiShineHalfWidth) * shine_strength;
        if (intensity > 0.0f) {
            rf += (255.0f - rf) * intensity;
            gf += (255.0f - gf) * intensity;
            bf += (255.0f - bf) * intensity;
        }
    }
    *r = (uint8_t)lroundf(rf);
    *g = (uint8_t)lroundf(gf);
    *b = (uint8_t)lroundf(bf);
}

/** 取 π 渐变上 t 处的 0xRRGGBB。
 *
 * 存在理由：Grobot 脸的情绪配色必须与开机 π 同一条渐变，否则开机交接后
 * 颜色会跳。让脸直接从这里取色，「和谐」就是结构不变量，而不是手抄的十六进制。
 */
inline uint32_t PiGradientHex(float t) {
    uint8_t r = 0, g = 0, b = 0;
    PiGradientRgb(t, 0.0f, 0.0f, &r, &g, &b);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/** 归一化到 [0,1) 的取模，等价上游 `((v % 1) + 1) % 1`。 */
inline float PiWrap01(float v) {
    float w = std::fmod(v, 1.0f);
    w = std::fmod(w + 1.0f, 1.0f);
    return w;
}

struct PiIntroFrame {
    float phase;           // 渐变沿对角线的相位偏移
    float shine_strength;  // 高光整体强度
    float shine_pos;       // 高光带中心位置
};

/** 上游 introLogoFrame()：ease-out cubic 减速，渐变倒转 2.5 圈后正好落在静止帧。 */
inline PiIntroFrame PiIntroFrameAt(float progress) {
    const float inv = 1.0f - progress;
    const float eased = 1.0f - inv * inv * inv;
    PiIntroFrame frame;
    frame.phase = PiWrap01((1.0f - eased) * kPiIntroSweeps);
    frame.shine_pos = PiWrap01(progress * kPiIntroShineTraversals);
    frame.shine_strength = std::pow(1.0f - eased, 1.5f);
    return frame;
}

/** 单元 (x, y) 的渐变位置 t：左下 → 右上对角线，叠加相位。 */
inline float PiCellGradientT(int x, int y, float phase) {
    const float base = (float)(x + (kPiLogoRowCount - 1 - y)) / (float)kPiLogoSpan;
    return PiWrap01(base + phase);
}

// 脸部空间渐变：Grobot 全脸铺与 logo 静止帧**完全相同**的 0..1 全程渐变
// （左下热粉→右上薄荷），肉眼可见的彩虹扫过是「和 π 一样」的定义——
// 窄窗口（如 0.45）在单只眼睛上只有 Δt≈0.05，实测与平色无区别，已否决。
// 情绪不再改窗口，而是给整段渐变加一个相位旋转（kMoodGradientT 经
// kPiFacePhaseSwing 压缩成 ±0.15）：情绪色仍可辨，但任何情绪下脸都是彩虹。
static constexpr float kPiFacePhaseSwing = 0.30f;

/** 脸部像素 (x, y) 的渐变位置：与 PiCellGradientT 同向（左下→右上），
 *  span 同样用 +1 技巧让 base 严格 < 1，绕回缝被推到画布远角之外，
 *  不会从五官中间劈出一条 mint|pink 硬边。 */
inline float PiFaceGradientT(int x, int y, int w, int h, float phase) {
    const float span = (float)(w + h - 1);
    const float base = ((float)x + (float)(h - 1 - y)) / span;
    return PiWrap01(base + phase);
}

/** 单元子矩形 → 画布整数像素范围。
 *
 * 取整规则必须只有这一处：板上渲染（hutuji_pi_splash.cc）、host 测试与
 * `tmp/pi_logo_preview.py` 共用它，才能保证预览图与屏幕逐像素一致。
 * 各边独立 lroundf 而非「起点 + 四舍五入宽度」——后者会让相邻单元出现
 * 1px 缝或重叠，π 的竖腿看上去会忽粗忽细。
 */
struct PiPixelRect {
    int x0, y0, x1, y1;
};
inline PiPixelRect PiRectToPixels(const PiCellRect& r, int cell_x, int cell_y, int cell_w,
                                  int cell_h) {
    PiPixelRect p;
    p.x0 = cell_x + (int)lroundf(r.x0 * (float)cell_w);
    p.y0 = cell_y + (int)lroundf(r.y0 * (float)cell_h);
    p.x1 = cell_x + (int)lroundf(r.x1 * (float)cell_w);
    p.y1 = cell_y + (int)lroundf(r.y1 * (float)cell_h);
    return p;
}

#endif  // HUTUJI_PI_SPLASH_CORE_H
