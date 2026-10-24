#include "hutuji_pi_splash.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>

#include "hutuji_pi_splash_core.h"

static const char* TAG = "PiSplash";

// 交接阶段时长：π 上浮淡出、Grobot 眼睛淡入上浮。上游没有这一段（omp intro
// 结束就停在静止帧），是本机为「π 渐变成 Grobot」新增的，故不属于一比一范围。
static constexpr int kHandoffMs = 500;
// 交接位移：π 上飘、脸自下浮起的像素量，取单元高的一半量级，够看清但不出框。
static constexpr int kHandoffLiftPx = 18;

HutujiPiSplash::~HutujiPiSplash() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (canvas_ != nullptr) {
        lv_obj_delete(canvas_);
        canvas_ = nullptr;
    }
    if (backdrop_ != nullptr) {
        lv_obj_delete(backdrop_);
        backdrop_ = nullptr;
    }
    if (draw_buf_ != nullptr) {
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
    }
    buf_ = nullptr;
}

bool HutujiPiSplash::Start(lv_obj_t* parent, int screen_w, int screen_h, lv_obj_t* reveal_target) {
    if (parent == nullptr || screen_w <= 0 || screen_h <= 0) {
        return false;
    }
    // 单元尺寸按屏幕反推，保留四周边距；终端字符格高宽比约 1:2，故 cell_h ≈ 1.5×cell_w。
    // 480x320 下得 cell 32x48 → logo 384x240，与预览脚本一致。
    cell_w_ = std::max(1, (screen_w * 4 / 5) / kPiLogoColCount);
    cell_h_ = std::max(1, (screen_h * 3 / 4) / kPiLogoRowCount);
    logo_w_ = cell_w_ * kPiLogoColCount;
    logo_h_ = cell_h_ * kPiLogoRowCount;

    draw_buf_ = lv_draw_buf_create(logo_w_, logo_h_, LV_COLOR_FORMAT_RGB565, 0);
    if (draw_buf_ == nullptr) {
        ESP_LOGW(TAG, "draw buf alloc failed (%dx%d), skip splash", logo_w_, logo_h_);
        return false;
    }

    // 不透明底遮住主界面：logo 只覆盖中心矩形，四周必须是纯黑，否则开机瞬间
    // 会先闪一帧完整主界面（按钮+脸），破坏「π 先出场」的观感。
    backdrop_ = lv_obj_create(parent);
    if (backdrop_ == nullptr) {
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
        return false;
    }
    lv_obj_remove_style_all(backdrop_);
    lv_obj_set_size(backdrop_, screen_w, screen_h);
    lv_obj_set_pos(backdrop_, 0, 0);
    lv_obj_set_style_bg_color(backdrop_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);

    canvas_ = lv_canvas_create(backdrop_);
    if (canvas_ == nullptr) {
        lv_obj_delete(backdrop_);
        backdrop_ = nullptr;
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
        return false;
    }
    lv_canvas_set_draw_buf(canvas_, draw_buf_);
    lv_obj_center(canvas_);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    buf_ = (uint16_t*)draw_buf_->data;

    reveal_target_ = reveal_target;
    if (reveal_target_ != nullptr) {
        // 交接前先把脸压成全透明，避免它在 π 播放期间从底下透出来。
        lv_obj_set_style_opa(reveal_target_, LV_OPA_TRANSP, 0);
    }

    start_us_ = esp_timer_get_time();
    RenderLogo(0.0f, 0.0f, 0.0f);
    timer_ = lv_timer_create(TimerCb, kPiIntroTickMs, this);
    if (timer_ == nullptr) {
        ESP_LOGW(TAG, "timer create failed, skip splash");
        Finish();
        return false;
    }
    ESP_LOGI(TAG, "pi splash started: logo %dx%d cell %dx%d intro %dms", logo_w_, logo_h_, cell_w_,
             cell_h_, kPiIntroMs);
    return true;
}

void HutujiPiSplash::TimerCb(lv_timer_t* t) {
    ((HutujiPiSplash*)lv_timer_get_user_data(t))->Tick();
}

void HutujiPiSplash::FillRect(int x, int y, int w, int h, uint16_t color) {
    const int stride = draw_buf_->header.stride / 2;
    const int x1 = std::min(logo_w_, x + w);
    const int y1 = std::min(logo_h_, y + h);
    for (int row = std::max(0, y); row < y1; row++) {
        uint16_t* line = buf_ + (size_t)row * stride;
        for (int col = std::max(0, x); col < x1; col++) {
            line[col] = color;
        }
    }
}

void HutujiPiSplash::RenderLogo(float phase, float shine_strength, float shine_pos) {
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    // fill_bg 可能重置 draw buf 指针（与 GrobotEyes 同一坑），每帧重取。
    buf_ = (uint16_t*)draw_buf_->data;
    PiCellRect rects[kPiCellRectMax];
    for (int y = 0; y < kPiLogoRowCount; y++) {
        const char* row = kPiLogoRows[y];
        for (int x = 0; x < kPiLogoColCount; x++) {
            const int count = PiCellRects(row[x], rects);
            if (count == 0) {
                continue;
            }
            uint8_t r = 0, g = 0, b = 0;
            PiGradientRgb(PiCellGradientT(x, y, phase), shine_strength, shine_pos, &r, &g, &b);
            const uint16_t color = lv_color_to_u16(lv_color_make(r, g, b));
            const int cx = x * cell_w_;
            const int cy = y * cell_h_;
            for (int i = 0; i < count; i++) {
                const PiPixelRect p = PiRectToPixels(rects[i], cx, cy, cell_w_, cell_h_);
                FillRect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0, color);
            }
        }
    }
    lv_obj_invalidate(canvas_);
}

void HutujiPiSplash::Tick() {
    const int64_t elapsed_ms = (esp_timer_get_time() - start_us_) / 1000;
    if (elapsed_ms < kPiIntroMs) {
        const PiIntroFrame frame = PiIntroFrameAt((float)elapsed_ms / (float)kPiIntroMs);
        RenderLogo(frame.phase, frame.shine_strength, frame.shine_pos);
        return;
    }
    const int64_t handoff_ms = elapsed_ms - kPiIntroMs;
    if (handoff_ms >= kHandoffMs) {
        Finish();
        return;
    }
    // 交接：progress 0→1，π 上浮淡出、脸淡入并自下浮到位。静止帧只渲染一次，
    // 之后每帧只改 opa/translate，省掉整幅重绘。
    if (handoff_ms < kPiIntroTickMs) {
        RenderLogo(0.0f, 0.0f, 0.0f);
    }
    const float progress = (float)handoff_ms / (float)kHandoffMs;
    const lv_opa_t fade_out = (lv_opa_t)lroundf((1.0f - progress) * 255.0f);
    lv_obj_set_style_opa(canvas_, fade_out, 0);
    lv_obj_set_style_bg_opa(backdrop_, fade_out, 0);
    lv_obj_set_style_translate_y(canvas_, -(int)lroundf(progress * (float)kHandoffLiftPx), 0);
    if (reveal_target_ != nullptr) {
        lv_obj_set_style_opa(reveal_target_, (lv_opa_t)lroundf(progress * 255.0f), 0);
        lv_obj_set_style_translate_y(reveal_target_,
                                     (int)lroundf((1.0f - progress) * (float)kHandoffLiftPx), 0);
    }
}

void HutujiPiSplash::Finish() {
    if (reveal_target_ != nullptr) {
        lv_obj_set_style_opa(reveal_target_, LV_OPA_COVER, 0);
        lv_obj_set_style_translate_y(reveal_target_, 0, 0);
        reveal_target_ = nullptr;
    }
    // 定时器可能正在自身回调里被删：LVGL 的 timer_deleted 标志覆盖这种自删。
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (canvas_ != nullptr) {
        lv_obj_delete(canvas_);
        canvas_ = nullptr;
    }
    if (backdrop_ != nullptr) {
        lv_obj_delete(backdrop_);
        backdrop_ = nullptr;
    }
    if (draw_buf_ != nullptr) {
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
    }
    buf_ = nullptr;
    ESP_LOGI(TAG, "pi splash finished, handed off to face");
}
