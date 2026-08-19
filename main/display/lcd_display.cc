/*
 * IMPECCABLE DIRECTION CONTRACT — hutuji-touchscreen-redesign
 * THESIS: Grobot is the creative companion; the machine never becomes a dense console.
 * OWN-WORLD: warm dark desk, soft paper cards, cyan attention, rounded controls.
 * FIRST VIEWPORT: full-screen Grobot, one quiet status pill, one control entry.
 * SIGNATURE: preview becomes a sheet of paper; debug tools unfold only on demand.
 * CONSTRAINTS: preserve callbacks, state gates, locale keys, and 56 px touch targets.
 */
#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <qrcode.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "board.h"
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
#include "boards/lichuang-dev/grobot_eyes.h"
#endif

#define TAG "LcdDisplay"

namespace {

void QrPixelCallback(esp_qrcode_handle_t qrcode, void* user_data) {
    auto* modules = static_cast<std::vector<uint8_t>*>(user_data);
    const int qr_size = esp_qrcode_get_size(qrcode);
    if (qr_size <= 0 || qr_size > 177) {
        modules->clear();
        return;
    }
    modules->resize(static_cast<size_t>(qr_size) * qr_size);
    for (int y = 0; y < qr_size; ++y) {
        for (int x = 0; x < qr_size; ++x) {
            (*modules)[static_cast<size_t>(y) * qr_size + x] =
                esp_qrcode_get_module(qrcode, x, y) ? 1 : 0;
        }
    }
}

std::unique_ptr<LvglImage> BuildProvisioningQrImage(const std::string& payload, int target_size) {
    std::vector<uint8_t> modules;
    esp_qrcode_config_t config = {
        .display_func_with_cb = QrPixelCallback,
        .max_qrcode_version = 8,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
        .user_data = &modules,
    };
    if (esp_qrcode_generate(&config, payload.c_str()) != ESP_OK || modules.empty()) {
        return nullptr;
    }

    const int qr_size = static_cast<int>(std::sqrt(modules.size()));
    const int full_size = qr_size + 8;  // ISO/IEC 18004 quiet zone: four modules per side.
    const int scale = target_size / full_size;
    if (qr_size <= 0 || scale <= 0) {
        return nullptr;
    }
    const int display_size = full_size * scale;
    const size_t bytes = static_cast<size_t>(display_size) * display_size * sizeof(uint16_t);
    auto* pixels = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (pixels == nullptr) {
        return nullptr;
    }

    constexpr uint16_t kForeground = 0x0000;
    constexpr uint16_t kBackground = 0xFFFF;
    for (int y = 0; y < display_size; ++y) {
        for (int x = 0; x < display_size; ++x) {
            const int module_x = x / scale - 4;
            const int module_y = y / scale - 4;
            const bool dark = module_x >= 0 && module_x < qr_size && module_y >= 0 &&
                              module_y < qr_size &&
                              modules[static_cast<size_t>(module_y) * qr_size + module_x] != 0;
            pixels[static_cast<size_t>(y) * display_size + x] = dark ? kForeground : kBackground;
        }
    }
    return std::make_unique<LvglAllocatedImage>(pixels, bytes, display_size, display_size,
                                                display_size * sizeof(uint16_t),
                                                LV_COLOR_FORMAT_RGB565);
}

}  // namespace

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme：暖纸底 + 墨青文字，供主题切换/资产包覆写使用。
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xF4F1EA));
    light_theme->set_text_color(lv_color_hex(0x1E252B));
    light_theme->set_chat_background_color(lv_color_hex(0xEDE8DE));
    light_theme->set_user_bubble_color(lv_color_hex(0xD8F3F0));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xFFFDF8));
    light_theme->set_system_bubble_color(lv_color_hex(0xF4F1EA));
    light_theme->set_system_text_color(lv_color_hex(0x667078));
    light_theme->set_border_color(lv_color_hex(0xD8D1C4));
    light_theme->set_low_battery_color(lv_color_hex(0xC64F52));
    light_theme->set_surface_color(lv_color_hex(0xFFFDF8));
    light_theme->set_muted_text_color(lv_color_hex(0x667078));
    light_theme->set_accent_color(lv_color_hex(0x0F8F8A));
    light_theme->set_accent_text_color(lv_color_hex(0xFFFFFF));
    light_theme->set_success_color(lv_color_hex(0x2F8F68));
    light_theme->set_warning_color(lv_color_hex(0xC8862A));
    light_theme->set_danger_color(lv_color_hex(0xC64F52));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme：低饱和墨蓝底，青蓝只做注意力，不用霓虹铺满屏幕。
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x10161C));
    dark_theme->set_text_color(lv_color_hex(0xF2F0E8));
    dark_theme->set_chat_background_color(lv_color_hex(0x141C22));
    dark_theme->set_user_bubble_color(lv_color_hex(0x1B7773));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x1E2930));
    dark_theme->set_system_bubble_color(lv_color_hex(0x172026));
    dark_theme->set_system_text_color(lv_color_hex(0xA7B1B8));
    dark_theme->set_border_color(lv_color_hex(0x34434A));
    dark_theme->set_low_battery_color(lv_color_hex(0xD95D62));
    dark_theme->set_surface_color(lv_color_hex(0x1A242B));
    dark_theme->set_muted_text_color(lv_color_hex(0xA7B1B8));
    dark_theme->set_accent_color(lv_color_hex(0x32D6CB));
    dark_theme->set_accent_text_color(lv_color_hex(0x071316));
    dark_theme->set_success_color(lv_color_hex(0x5ECB9A));
    dark_theme->set_warning_color(lv_color_hex(0xE4B15D));
    dark_theme->set_danger_color(lv_color_hex(0xE06A70));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    std::string theme_name = "dark";
    settings.SetString("theme", "dark");
#else
    std::string theme_name = settings.GetString("theme", "dark");
#endif
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}
void LcdDisplay::EnsureProvisioningQrUi() {
    if (provisioning_qr_root_ != nullptr) {
        return;
    }

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    provisioning_qr_root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(provisioning_qr_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(provisioning_qr_root_, 0, 0);
    lv_obj_set_style_border_width(provisioning_qr_root_, 0, 0);
    lv_obj_set_style_bg_color(provisioning_qr_root_, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(provisioning_qr_root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(provisioning_qr_root_, 0, 0);
    lv_obj_set_flex_flow(provisioning_qr_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(provisioning_qr_root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(provisioning_qr_root_, theme->spacing(4), 0);
    lv_obj_clear_flag(provisioning_qr_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(provisioning_qr_root_, LV_OBJ_FLAG_CLICKABLE);

    provisioning_qr_code_ = lv_image_create(provisioning_qr_root_);
    provisioning_qr_hint_ = lv_label_create(provisioning_qr_root_);
    lv_obj_set_width(provisioning_qr_hint_, LV_HOR_RES - theme->spacing(8));
    lv_obj_set_style_text_font(provisioning_qr_hint_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(provisioning_qr_hint_, theme->text_color(), 0);
    lv_obj_set_style_text_align(provisioning_qr_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(provisioning_qr_hint_, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::ShowProvisioningQr(const std::string& payload, const std::string& hint) {
    DisplayLockGuard lock(this);
    EnsureProvisioningQrUi();
    if (provisioning_qr_root_ == nullptr) {
        return;
    }

    const int target_size = height_ >= 300 ? 200 : 150;
    auto image = BuildProvisioningQrImage(payload, target_size);
    if (image == nullptr) {
        ESP_LOGE(TAG, "Failed to generate provisioning QR code");
        lv_obj_add_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(provisioning_qr_code_, nullptr);
        provisioning_qr_image_.reset();
        return;
    }

    lv_image_set_src(provisioning_qr_code_, nullptr);
    provisioning_qr_image_ = std::move(image);
    lv_image_set_src(provisioning_qr_code_, provisioning_qr_image_->image_dsc());
    if (machine_control_trigger_btn_ != nullptr) {
        lv_obj_add_flag(machine_control_trigger_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    if (machine_control_root_ != nullptr) {
        lv_obj_add_flag(machine_control_root_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(provisioning_qr_root_);
    lv_obj_remove_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::HideProvisioningQr() {
    DisplayLockGuard lock(this);
    if (provisioning_qr_root_ == nullptr) {
        return;
    }
    lv_obj_add_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN);
    if (machine_control_trigger_btn_ != nullptr &&
        (draw_preview_root_ == nullptr ||
         lv_obj_has_flag(draw_preview_root_, LV_OBJ_FLAG_HIDDEN))) {
        lv_obj_remove_flag(machine_control_trigger_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_src(provisioning_qr_code_, nullptr);
    provisioning_qr_image_.reset();
}

void LcdDisplay::EnsureDrawPreviewUi() {
    if (draw_preview_root_ != nullptr) {
        return;
    }

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    // 预览是一张纸：外圈暗场只负责压暗 Grobot，卡片内部留暖白纸面和两行操作区。
    // 480x320 预算：卡片 464x304，内边距 10，标题约 28，按钮 64，剩余约 190 给图片。
    // 所有可点目标高度 >= 56px（儿童手指命中面）。
    draw_preview_root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(draw_preview_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(draw_preview_root_, 0, 0);
    lv_obj_set_style_border_width(draw_preview_root_, 0, 0);
    lv_obj_set_style_bg_color(draw_preview_root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(draw_preview_root_, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(draw_preview_root_, theme->spacing(4), 0);
    lv_obj_set_flex_flow(draw_preview_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(draw_preview_root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(draw_preview_root_, LV_OBJ_FLAG_SCROLLABLE);
    // 与二维码层同款：整层可点击，吃掉落到情绪层/聊天层的误触。
    lv_obj_add_flag(draw_preview_root_, LV_OBJ_FLAG_CLICKABLE);

    draw_preview_card_ = lv_obj_create(draw_preview_root_);
    const lv_coord_t card_width = LV_HOR_RES - theme->spacing(8);
    const lv_coord_t card_height = LV_VER_RES - theme->spacing(8);
    const lv_coord_t content_width = card_width - theme->spacing(10);
    lv_obj_set_size(draw_preview_card_, card_width, card_height);
    lv_obj_set_style_radius(draw_preview_card_, 24, 0);
    lv_obj_set_style_bg_color(draw_preview_card_, theme->surface_color(), 0);
    lv_obj_set_style_bg_opa(draw_preview_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(draw_preview_card_, 1, 0);
    lv_obj_set_style_border_color(draw_preview_card_, theme->border_color(), 0);
    lv_obj_set_style_shadow_width(draw_preview_card_, 24, 0);
    lv_obj_set_style_shadow_color(draw_preview_card_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(draw_preview_card_, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(draw_preview_card_, theme->spacing(5), 0);
    lv_obj_set_style_pad_row(draw_preview_card_, theme->spacing(3), 0);
    lv_obj_set_flex_flow(draw_preview_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(draw_preview_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(draw_preview_card_, LV_OBJ_FLAG_SCROLLABLE);

    // 标题即提示语，置顶单行打点截断：再长的文案也不挤压图片与按钮。
    // 字体继承屏幕级主题字体（SetTheme 会随主题刷新屏幕字体），不持有裸指针。
    draw_preview_hint_ = lv_label_create(draw_preview_card_);
    lv_obj_set_width(draw_preview_hint_, content_width);
    lv_obj_set_style_text_color(draw_preview_hint_, theme->text_color(), 0);
    lv_obj_set_style_text_align(draw_preview_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(draw_preview_hint_, LV_LABEL_LONG_DOT);

    draw_preview_image_ = lv_image_create(draw_preview_card_);
    lv_obj_set_style_bg_color(draw_preview_image_, lv_color_hex(0xF6F1E6), 0);
    lv_obj_set_style_bg_opa(draw_preview_image_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(draw_preview_image_, 16, 0);
    lv_obj_set_style_border_width(draw_preview_image_, 1, 0);
    lv_obj_set_style_border_color(draw_preview_image_, lv_color_hex(0xD8D1C4), 0);
    lv_obj_set_style_pad_all(draw_preview_image_, theme->spacing(2), 0);

    // 按钮行沉底：「开始画」是唯一主动作，占 2/3 宽；「取消」次动作占 1/3。
    // 行高取屏高 20%（480x320 下 64px）且不低于 56px。
    lv_obj_t* button_row = lv_obj_create(draw_preview_card_);
    lv_obj_remove_style_all(button_row);
    lv_obj_set_width(button_row, content_width);
    lv_obj_set_height(button_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(button_row, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t button_height = LV_VER_RES * 20 / 100 < 56 ? 56 : LV_VER_RES * 20 / 100;
    const lv_coord_t row_width = content_width;
    const lv_coord_t confirm_width = (row_width - theme->spacing(4)) * 2 / 3;
    const lv_coord_t cancel_width = row_width - theme->spacing(4) - confirm_width;

    auto make_button = [&](const char* text, lv_color_t color, lv_color_t text_color,
                           lv_coord_t width) {
        lv_obj_t* btn = lv_button_create(button_row);
        lv_obj_set_size(btn, width, button_height);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_bg_color(btn, color, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, text_color, 0);
        lv_obj_center(label);
        return btn;
    };

    // 青色大按钮=开始画，灰色小按钮=取消：不靠文字识读也能分辨主次。
    draw_preview_confirm_btn_ =
        make_button(Lang::Strings::DRAW_PREVIEW_CONFIRM, theme->accent_color(),
                    theme->accent_text_color(), confirm_width);
    draw_preview_cancel_btn_ =
        make_button(Lang::Strings::DRAW_PREVIEW_CANCEL, theme->assistant_bubble_color(),
                    theme->text_color(), cancel_width);

    // 回调在 LVGL 任务里跑，只做转发：Job 侧建任务/发通知都不阻塞。
    lv_obj_add_event_cb(
        draw_preview_confirm_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            if (self->draw_preview_on_confirm_) {
                self->draw_preview_on_confirm_();
            }
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        draw_preview_cancel_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            if (self->draw_preview_on_cancel_) {
                self->draw_preview_on_cancel_();
            }
        },
        LV_EVENT_PRESSED, this);

    lv_obj_add_flag(draw_preview_root_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::ShowDrawPreview(std::unique_ptr<LvglImage> image, const std::string& hint,
                                 std::function<void()> on_confirm,
                                 std::function<void()> on_cancel) {
    DisplayLockGuard lock(this);
    if (image == nullptr) {
        return;
    }
    EnsureDrawPreviewUi();
    if (draw_preview_root_ == nullptr) {
        return;
    }

    // 先摘旧 src 再换缓存：LVGL 仍持有旧 dsc 指针时释放会画到野内存。
    lv_image_set_src(draw_preview_image_, nullptr);
    draw_preview_cached_ = std::move(image);
    auto* img_dsc = draw_preview_cached_->image_dsc();
    lv_image_set_src(draw_preview_image_, img_dsc);

    // 与 EnsureDrawPreviewUi 的卡片预算一致：内容宽 = 卡片宽 - 两侧内边距 20；
    // 高 = 卡片高 - 上下内边距 20、标题约 28、按钮行与两处行距，其余全部留给图片。
    // 等比放大不超过原始像素。
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const lv_coord_t button_height = LV_VER_RES * 20 / 100 < 56 ? 56 : LV_VER_RES * 20 / 100;
    const lv_coord_t max_width = LV_HOR_RES - theme->spacing(18);
    const lv_coord_t max_height =
        LV_VER_RES - theme->spacing(18) - 28 - button_height - theme->spacing(6);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        const lv_coord_t zoom_w = (max_width * 256) / img_dsc->header.w;
        const lv_coord_t zoom_h = (max_height * 256) / img_dsc->header.h;
        lv_coord_t zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
        if (zoom > 256) {
            zoom = 256;
        }
        lv_image_set_scale(draw_preview_image_, zoom);
        lv_obj_set_size(draw_preview_image_, (img_dsc->header.w * zoom) / 256,
                        (img_dsc->header.h * zoom) / 256);
    }

    lv_label_set_text(draw_preview_hint_, hint.c_str());
    draw_preview_on_confirm_ = std::move(on_confirm);
    draw_preview_on_cancel_ = std::move(on_cancel);
    if (machine_control_trigger_btn_ != nullptr) {
        lv_obj_add_flag(machine_control_trigger_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    if (machine_control_root_ != nullptr) {
        lv_obj_add_flag(machine_control_root_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(draw_preview_root_);
    lv_obj_remove_flag(draw_preview_root_, LV_OBJ_FLAG_HIDDEN);
    // 配网二维码优先级更高：配网中弹预览不得盖住二维码。
    if (provisioning_qr_root_ != nullptr &&
        !lv_obj_has_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(provisioning_qr_root_);
    }
}

void LcdDisplay::HideDrawPreview() {
    DisplayLockGuard lock(this);
    if (draw_preview_root_ == nullptr) {
        return;
    }
    lv_obj_add_flag(draw_preview_root_, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(draw_preview_image_, nullptr);
    draw_preview_cached_.reset();
    // 回调置空必须与撤图同锁：否则按钮迟到点击会打到已失效的 Job 状态。
    draw_preview_on_confirm_ = nullptr;
    if (machine_control_trigger_btn_ != nullptr &&
        (provisioning_qr_root_ == nullptr ||
         lv_obj_has_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN))) {
        lv_obj_remove_flag(machine_control_trigger_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    draw_preview_on_cancel_ = nullptr;
}

void LcdDisplay::EnsureMachineControlUi() {
    if (!machine_controls_configured_ || !setup_ui_called_ ||
        machine_control_trigger_btn_ != nullptr) {
        return;
    }

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    // 480x320 横屏：主操作行取屏高 20%（64px），其余按钮行取 17%（54px），
    // 一律按 56px 下限兜底，保证儿童手指命中面。
    const lv_coord_t primary_height = LV_VER_RES * 20 / 100 < 56 ? 56 : LV_VER_RES * 20 / 100;
    const lv_coord_t button_height = LV_VER_RES * 17 / 100;
    const lv_coord_t safe_button_height = button_height < 56 ? 56 : button_height;
    // disabled 用实心灰底+实心浅字：可辨靠的是填充色差异，不靠低对比文字。
    const lv_color_t disabled_bg = lv_color_hex(0x536069);
    const lv_color_t disabled_text = lv_color_hex(0xD8DEE2);
    auto* screen = lv_screen_active();

    // 控制入口：主屏只留这一个右侧胶囊，青蓝品牌色承担唯一视觉锚点。
    machine_control_trigger_btn_ = lv_button_create(screen);
    lv_obj_set_size(machine_control_trigger_btn_, LV_HOR_RES * 24 / 100, primary_height);
    lv_obj_align(machine_control_trigger_btn_, LV_ALIGN_RIGHT_MID, -theme->spacing(3), 0);
    lv_obj_set_style_radius(machine_control_trigger_btn_, 24, 0);
    lv_obj_set_style_bg_color(machine_control_trigger_btn_, theme->accent_color(), 0);
    lv_obj_set_style_bg_opa(machine_control_trigger_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(machine_control_trigger_btn_, 1, 0);
    lv_obj_set_style_border_color(machine_control_trigger_btn_, theme->border_color(), 0);
    lv_obj_set_style_shadow_width(machine_control_trigger_btn_, 18, 0);
    lv_obj_set_style_shadow_color(machine_control_trigger_btn_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(machine_control_trigger_btn_, LV_OPA_30, 0);
    lv_obj_t* trigger_label = lv_label_create(machine_control_trigger_btn_);
    lv_label_set_text(trigger_label, Lang::Strings::MACHINE_CONTROL);
    lv_obj_set_style_text_color(trigger_label, theme->accent_text_color(), 0);
    lv_obj_center(trigger_label);

    // 抽屉：暗场压底 + 居中纸感面板；点遮罩空白处收起（保留原有语义）。
    machine_control_root_ = lv_obj_create(screen);
    lv_obj_set_size(machine_control_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(machine_control_root_, 0, 0);
    lv_obj_set_style_border_width(machine_control_root_, 0, 0);
    lv_obj_set_style_bg_color(machine_control_root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(machine_control_root_, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(machine_control_root_, theme->spacing(4), 0);
    lv_obj_clear_flag(machine_control_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(machine_control_root_, LV_OBJ_FLAG_CLICKABLE);

    // 面板 448 宽、内容高、最高贴屏内边；展开调试后允许面板内部垂直滚动。
    const lv_coord_t panel_width = LV_HOR_RES - theme->spacing(16);
    const lv_coord_t content_width = panel_width - theme->spacing(8);
    const lv_coord_t half_width = (content_width - theme->spacing(4)) / 2;
    lv_obj_t* panel = lv_obj_create(machine_control_root_);
    lv_obj_set_width(panel, panel_width);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LV_VER_RES - theme->spacing(4), 0);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, theme->border_color(), 0);
    lv_obj_set_style_bg_color(panel, theme->surface_color(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 24, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(panel, theme->spacing(4), 0);
    lv_obj_set_style_pad_row(panel, theme->spacing(4), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    auto make_row = [&](lv_obj_t* parent) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, content_width, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        return row;
    };

    // 字体继承屏幕级主题字体（SetTheme 随主题刷新），不持有裸指针。
    auto make_button = [&](lv_obj_t* parent, const char* text, lv_color_t color,
                           lv_color_t text_color, lv_coord_t width, lv_coord_t height,
                           lv_obj_t** label_out = nullptr) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, width, height);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_bg_color(btn, color, 0);
        lv_obj_set_style_bg_color(btn, disabled_bg, LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, text_color, 0);
        lv_obj_set_style_text_color(label, disabled_text, LV_STATE_DISABLED);
        lv_obj_center(label);
        if (label_out != nullptr) {
            *label_out = label;
        }
        return btn;
    };

    // 标题行：抽屉名 + 当前机器状态 + 收起，一眼看清“我在哪、机器在干嘛”。
    lv_obj_t* header = make_row(panel);
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, Lang::Strings::MACHINE_DRAWER_TITLE);
    lv_obj_set_style_text_color(title, theme->text_color(), 0);
    machine_state_label_ = lv_label_create(header);
    lv_label_set_text(machine_state_label_, "");
    lv_obj_set_style_bg_color(machine_state_label_, theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(machine_state_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(machine_state_label_, 12, 0);
    lv_obj_set_style_pad_left(machine_state_label_, theme->spacing(3), 0);
    lv_obj_set_style_pad_right(machine_state_label_, theme->spacing(3), 0);
    lv_obj_set_style_pad_top(machine_state_label_, theme->spacing(1), 0);
    lv_obj_set_style_pad_bottom(machine_state_label_, theme->spacing(1), 0);
    machine_close_btn_ =
        make_button(header, Lang::Strings::MACHINE_CLOSE, theme->assistant_bubble_color(),
                    theme->text_color(), LV_HOR_RES / 5, safe_button_height);

    // 主操作行：暂停/继续同位二选一，Job 状态保证任一时刻只有一个可用；
    // 可用者保持实心高亮，即当前唯一主动作。
    lv_obj_t* primary_row = make_row(panel);
    machine_pause_btn_ =
        make_button(primary_row, Lang::Strings::MACHINE_PAUSE, theme->warning_color(),
                    lv_color_hex(0x14110A), half_width, primary_height);
    machine_resume_btn_ =
        make_button(primary_row, Lang::Strings::MACHINE_RESUME, theme->success_color(),
                    lv_color_hex(0x071510), half_width, primary_height);

    // 次级行：再画一张/试试笔，仅结束态可用。
    lv_obj_t* secondary_row = make_row(panel);
    machine_repeat_btn_ =
        make_button(secondary_row, Lang::Strings::MACHINE_REPEAT, theme->accent_color(),
                    theme->accent_text_color(), half_width, safe_button_height);
    machine_pen_test_btn_ =
        make_button(secondary_row, Lang::Strings::MACHINE_PEN_TEST, lv_color_hex(0x5F7180),
                    lv_color_white(), half_width, safe_button_height);

    // 停止：危险动作独占末行，红色实心作语义警示，不与主操作行争视觉重心。
    machine_abort_btn_ = make_button(panel, Lang::Strings::MACHINE_STOP, theme->danger_color(),
                                     lv_color_white(), content_width, safe_button_height);

    // 高级调试默认折叠：孩子看到的是创作面板，点动/复位等工具仍在但不做主界面噪音。
    machine_manual_toggle_btn_ = make_button(
        panel, Lang::Strings::MACHINE_MANUAL_EXPAND, theme->assistant_bubble_color(),
        theme->text_color(), content_width, safe_button_height, &machine_manual_toggle_label_);

    machine_manual_section_ = lv_obj_create(panel);
    lv_obj_remove_style_all(machine_manual_section_);
    lv_obj_set_size(machine_manual_section_, content_width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(machine_manual_section_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(machine_manual_section_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(machine_manual_section_, theme->spacing(3), 0);
    lv_obj_clear_flag(machine_manual_section_, LV_OBJ_FLAG_SCROLLABLE);

    // ── 手动调试区（2026-08-18 用户决策全量开放；命令逐字对齐奎享实测序列）──
    // 动作不经 drawer 收起：点动/抬落笔需要连续操作。可用性统一由
    // ApplyMachineControlState 按 settled 态门控。
    lv_obj_t* manual_label = lv_label_create(machine_manual_section_);
    lv_label_set_text(manual_label, Lang::Strings::MACHINE_MANUAL_SECTION);
    lv_obj_set_style_text_color(manual_label, theme->muted_text_color(), 0);

    const lv_coord_t third_width = (content_width - theme->spacing(8)) / 3;
    auto make_manual = [&](lv_obj_t* parent, const char* text, lv_color_t color,
                           lv_color_t text_color, lv_coord_t width, const char* action) {
        lv_obj_t* btn = make_button(parent, text, color, text_color, width, safe_button_height);
        lv_obj_set_user_data(btn, const_cast<char*>(action));
        machine_manual_buttons_.push_back(btn);
        return btn;
    };

    lv_obj_t* pen_row = make_row(machine_manual_section_);
    make_manual(pen_row, Lang::Strings::MACHINE_PEN_UP, theme->accent_color(),
                theme->accent_text_color(), half_width, "pen_up");
    make_manual(pen_row, Lang::Strings::MACHINE_PEN_DOWN, theme->accent_color(),
                theme->accent_text_color(), half_width, "pen_down");

    // 点动十字（对齐奎享面板）：Y+ 居上，X-/回原点/X+ 居中行，Y- 居下。
    lv_obj_t* jog_up_row = make_row(machine_manual_section_);
    lv_obj_set_flex_align(jog_up_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_manual(jog_up_row, Lang::Strings::MACHINE_JOG_YP, theme->assistant_bubble_color(),
                theme->text_color(), third_width, "jog_y+");
    lv_obj_t* jog_mid_row = make_row(machine_manual_section_);
    make_manual(jog_mid_row, Lang::Strings::MACHINE_JOG_XM, theme->assistant_bubble_color(),
                theme->text_color(), third_width, "jog_x-");
    make_manual(jog_mid_row, Lang::Strings::MACHINE_HOME, theme->success_color(),
                lv_color_hex(0x071510), third_width, "home");
    make_manual(jog_mid_row, Lang::Strings::MACHINE_JOG_XP, theme->assistant_bubble_color(),
                theme->text_color(), third_width, "jog_x+");
    lv_obj_t* jog_down_row = make_row(machine_manual_section_);
    lv_obj_set_flex_align(jog_down_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_manual(jog_down_row, Lang::Strings::MACHINE_JOG_YM, theme->assistant_bubble_color(),
                theme->text_color(), third_width, "jog_y-");

    lv_obj_t* origin_row = make_row(machine_manual_section_);
    make_manual(origin_row, Lang::Strings::MACHINE_SET_ORIGIN, theme->warning_color(),
                lv_color_hex(0x14110A), half_width, "set_origin");
    make_manual(origin_row, Lang::Strings::MACHINE_UNLOCK, theme->assistant_bubble_color(),
                theme->text_color(), half_width, "unlock");

    lv_obj_t* power_row = make_row(machine_manual_section_);
    make_manual(power_row, Lang::Strings::MACHINE_MOTOR_OFF, theme->assistant_bubble_color(),
                theme->text_color(), half_width, "motor_off");
    make_manual(power_row, Lang::Strings::MACHINE_RESET, theme->danger_color(), lv_color_white(),
                half_width, "reset");
    SetMachineManualSectionVisible(false);

    lv_obj_add_event_cb(
        machine_control_trigger_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            ESP_LOGI(TAG, "machine controls opened");
            self->SetMachineManualSectionVisible(false);
            self->ApplyMachineControlState();
            lv_obj_move_foreground(self->machine_control_root_);
            lv_obj_remove_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_manual_toggle_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            self->SetMachineManualSectionVisible(
                lv_obj_has_flag(self->machine_manual_section_, LV_OBJ_FLAG_HIDDEN));
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_pause_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
            if (self->machine_pause_)
                self->machine_pause_();
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_resume_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
            if (self->machine_resume_)
                self->machine_resume_();
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_abort_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
            if (self->machine_abort_)
                self->machine_abort_();
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_repeat_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
            if (self->machine_repeat_)
                self->machine_repeat_();
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_pen_test_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
            if (self->machine_pen_test_)
                self->machine_pen_test_();
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_close_btn_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
        },
        LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(
        machine_control_root_,
        [](lv_event_t* e) {
            auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            if (lv_event_get_target(e) == self->machine_control_root_) {
                lv_obj_add_flag(self->machine_control_root_, LV_OBJ_FLAG_HIDDEN);
            }
        },
        LV_EVENT_PRESSED, this);
    for (lv_obj_t* btn : machine_manual_buttons_) {
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
                const char* action = static_cast<const char*>(
                    lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_current_target(e))));
                if (self->machine_manual_ && action != nullptr) {
                    self->machine_manual_(action);
                }
            },
            LV_EVENT_PRESSED, this);
    }

    lv_obj_add_flag(machine_control_root_, LV_OBJ_FLAG_HIDDEN);
    ApplyMachineControlState();
}

void LcdDisplay::SetMachineManualSectionVisible(bool visible) {
    if (machine_manual_section_ == nullptr || machine_manual_toggle_label_ == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(machine_manual_section_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(machine_manual_toggle_label_, Lang::Strings::MACHINE_MANUAL_COLLAPSE);
    } else {
        lv_obj_add_flag(machine_manual_section_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(machine_manual_toggle_label_, Lang::Strings::MACHINE_MANUAL_EXPAND);
    }
}

void LcdDisplay::ApplyMachineControlState() {
    if (machine_pause_btn_ == nullptr) {
        return;
    }
    const bool streaming = machine_state_ == "streaming";
    const bool paused = machine_state_ == "paused";
    const bool active = streaming || paused || machine_state_ == "previewing" ||
                        machine_state_ == "awaiting_confirmation" ||
                        machine_state_ == "downloading" || machine_state_ == "verifying" ||
                        machine_state_ == "reconnecting" || machine_state_ == "paper_change" ||
                        machine_state_ == "pen_test";
    const bool settled = machine_state_ == "idle" || machine_state_ == "done" ||
                         machine_state_ == "error" || machine_state_ == "aborted";
    auto set_enabled = [](lv_obj_t* button, bool enabled) {
        if (enabled) {
            lv_obj_remove_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    };
    set_enabled(machine_pause_btn_, streaming);
    set_enabled(machine_resume_btn_, paused);
    set_enabled(machine_abort_btn_, active);
    set_enabled(machine_repeat_btn_, settled);
    set_enabled(machine_pen_test_btn_, settled);
    // 手动调试按钮（含复位/设置原点等高危项）仅 settled 态可用；manual 态自身也灰。
    for (lv_obj_t* btn : machine_manual_buttons_) {
        set_enabled(btn, settled);
    }

    // 标题行状态灯：文案走 locale，颜色复用按钮语义色（进行中=琥珀、
    // 完成=绿、出错=红、空闲/已停止=柔和正文色），纯展示不改 Job 契约。
    if (machine_state_label_ != nullptr) {
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        const char* state_text = Lang::Strings::MACHINE_STATE_BUSY;
        lv_color_t state_color = theme->warning_color();
        if (streaming) {
            state_text = Lang::Strings::MACHINE_STATE_STREAMING;
        } else if (paused) {
            state_text = Lang::Strings::MACHINE_STATE_PAUSED;
        } else if (machine_state_ == "done") {
            state_text = Lang::Strings::MACHINE_STATE_DONE;
            state_color = theme->success_color();
        } else if (machine_state_ == "manual") {
            state_text = Lang::Strings::MACHINE_STATE_MANUAL;
        } else if (machine_state_ == "error") {
            state_text = Lang::Strings::MACHINE_STATE_ERROR;
            state_color = theme->danger_color();
        } else if (machine_state_ == "idle" || machine_state_ == "aborted") {
            state_text = machine_state_ == "idle" ? Lang::Strings::MACHINE_STATE_IDLE
                                                  : Lang::Strings::MACHINE_STATE_ABORTED;
            state_color = theme->muted_text_color();
        }
        lv_label_set_text(machine_state_label_, state_text);
        lv_obj_set_style_text_color(machine_state_label_, state_color, 0);
        lv_obj_set_style_bg_color(machine_state_label_, theme->assistant_bubble_color(), 0);
    }
}

void LcdDisplay::ConfigureMachineControls(std::function<void()> on_pause,
                                          std::function<void()> on_resume,
                                          std::function<void()> on_abort,
                                          std::function<void()> on_repeat,
                                          std::function<void()> on_pen_test,
                                          std::function<void(const char* action)> on_manual) {
    machine_pause_ = std::move(on_pause);
    machine_resume_ = std::move(on_resume);
    machine_abort_ = std::move(on_abort);
    machine_repeat_ = std::move(on_repeat);
    machine_pen_test_ = std::move(on_pen_test);
    machine_manual_ = std::move(on_manual);
    machine_controls_configured_ = true;
    if (setup_ui_called_) {
        DisplayLockGuard lock(this);
        EnsureMachineControlUi();
    }
}

void LcdDisplay::UpdateMachineControlState(const std::string& state) {
    DisplayLockGuard lock(this);
    machine_state_ = state;
    ApplyMachineControlState();
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }
    if (provisioning_qr_root_ != nullptr) {
        lv_obj_del(provisioning_qr_root_);
        provisioning_qr_root_ = nullptr;
        provisioning_qr_code_ = nullptr;
        provisioning_qr_hint_ = nullptr;
        provisioning_qr_image_.reset();
    }
    if (machine_control_root_ != nullptr) {
        lv_obj_del(machine_control_root_);
        machine_control_root_ = nullptr;
        machine_pause_btn_ = nullptr;
        machine_resume_btn_ = nullptr;
        machine_abort_btn_ = nullptr;
        machine_repeat_btn_ = nullptr;
        machine_pen_test_btn_ = nullptr;
        machine_close_btn_ = nullptr;
        machine_state_label_ = nullptr;
    }
    if (machine_control_trigger_btn_ != nullptr) {
        lv_obj_del(machine_control_trigger_btn_);
        machine_control_trigger_btn_ = nullptr;
    }

    if (draw_preview_root_ != nullptr) {
        lv_obj_del(draw_preview_root_);
        draw_preview_root_ = nullptr;
        draw_preview_image_ = nullptr;
        draw_preview_hint_ = nullptr;
        draw_preview_cached_.reset();
    }
    if (grobot_subtitle_bar_ != nullptr) {
        lv_obj_del(grobot_subtitle_bar_);
        grobot_subtitle_bar_ = nullptr;
        grobot_subtitle_label_ = nullptr;
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }
void LcdDisplay::InitializeEmotionUi(lv_obj_t* screen, LvglTheme* theme,
                                     const lv_font_t* large_icon_font) {
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5 && CONFIG_HUTUJI_GROBOT_FACE
    // 纸感舞台放在 Grobot 画布后面；画布自身仍保持整屏透明/黑色底，避免圆角裁切闪烁。
    grobot_stage_ = lv_obj_create(screen);
    lv_obj_set_size(grobot_stage_, 468, 308);
    lv_obj_align(grobot_stage_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(grobot_stage_, 32, 0);
    lv_obj_set_style_bg_color(grobot_stage_, theme->surface_color(), 0);
    lv_obj_set_style_bg_opa(grobot_stage_, LV_OPA_60, 0);
    lv_obj_set_style_border_width(grobot_stage_, 1, 0);
    lv_obj_set_style_border_color(grobot_stage_, theme->border_color(), 0);
    lv_obj_set_style_shadow_width(grobot_stage_, 18, 0);
    lv_obj_set_style_shadow_color(grobot_stage_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(grobot_stage_, LV_OPA_20, 0);
    lv_obj_clear_flag(grobot_stage_, LV_OBJ_FLAG_SCROLLABLE);
#endif

    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_EDIT_SQUARE);

    emoji_image_ = lv_image_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    auto eye_color = theme->accent_color();
    auto eyes = std::make_unique<GrobotEyes>(eye_color, theme->background_color());
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5
    // 480x320 横屏：四边仅留约 10px 安全边距，状态栏继续独立叠在最前层。
    constexpr int kFaceWidth = 460;
    constexpr int kFaceHeight = 300;
#else
    constexpr int kFaceWidth = 280;
    constexpr int kFaceHeight = 190;
#endif
    lv_obj_set_size(emoji_box_, kFaceWidth, kFaceHeight);
    if (eyes->Init(emoji_box_, kFaceWidth, kFaceHeight)) {
        grobot_eyes_ = std::move(eyes);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        grobot_subtitle_bar_ = lv_obj_create(screen);
        lv_obj_set_size(grobot_subtitle_bar_, LV_HOR_RES * 72 / 100, 40);
        lv_obj_align(grobot_subtitle_bar_, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_radius(grobot_subtitle_bar_, 20, 0);
        lv_obj_set_style_border_width(grobot_subtitle_bar_, 1, 0);
        lv_obj_set_style_border_color(grobot_subtitle_bar_, theme->border_color(), 0);
        lv_obj_set_style_bg_color(grobot_subtitle_bar_, theme->surface_color(), 0);
        lv_obj_set_style_bg_opa(grobot_subtitle_bar_, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(grobot_subtitle_bar_, 0, 0);
        lv_obj_clear_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_SCROLLABLE);
        grobot_subtitle_label_ = lv_label_create(grobot_subtitle_bar_);
        lv_obj_set_width(grobot_subtitle_label_, LV_HOR_RES * 72 / 100 - 24);
        lv_label_set_long_mode(grobot_subtitle_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(grobot_subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(grobot_subtitle_label_, theme->text_color(), 0);
        lv_label_set_text(grobot_subtitle_label_, "");
        lv_obj_center(grobot_subtitle_label_);
        lv_obj_add_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "GrobotEyes initialized: %dx%d", kFaceWidth, kFaceHeight);
    } else {
        ESP_LOGE(TAG, "Failed to initialize GrobotEyes; using emoji fallback");
    }
#endif
}

void LcdDisplay::SetGrobotSubtitle(const char* content) {
    if (grobot_subtitle_bar_ == nullptr || grobot_subtitle_label_ == nullptr) {
        return;
    }
    const bool has_content = content != nullptr && content[0] != '\0';
    lv_label_set_text(grobot_subtitle_label_, has_content ? content : "");
    const bool overlay_visible = (provisioning_qr_root_ != nullptr &&
                                  !lv_obj_has_flag(provisioning_qr_root_, LV_OBJ_FLAG_HIDDEN)) ||
                                 (draw_preview_root_ != nullptr &&
                                  !lv_obj_has_flag(draw_preview_root_, LV_OBJ_FLAG_HIDDEN)) ||
                                 (machine_control_root_ != nullptr &&
                                  !lv_obj_has_flag(machine_control_root_, LV_OBJ_FLAG_HIDDEN));
    if (has_content && !hide_subtitle_ && !overlay_visible) {
        lv_obj_move_foreground(grobot_subtitle_bar_);
        lv_obj_remove_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(3), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(3), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(5), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(5), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    // 状态只保留一枚居中软胶囊；网络/电池图标留在两侧，避免桌面式密集栏。
    auto style_status_pill = [lvgl_theme](lv_obj_t* label) {
        lv_obj_set_width(label, LV_HOR_RES * 56 / 100);
        lv_obj_set_style_bg_color(label, lvgl_theme->surface_color(), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
        lv_obj_set_style_radius(label, 18, 0);
        lv_obj_set_style_border_width(label, 1, 0);
        lv_obj_set_style_border_color(label, lvgl_theme->border_color(), 0);
        lv_obj_set_style_pad_left(label, lvgl_theme->spacing(5), 0);
        lv_obj_set_style_pad_right(label, lvgl_theme->spacing(5), 0);
        lv_obj_set_style_pad_top(label, lvgl_theme->spacing(2), 0);
        lv_obj_set_style_pad_bottom(label, lvgl_theme->spacing(2), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lvgl_theme->text_color(), 0);
    };

    notification_label_ = lv_label_create(status_bar_);
    style_status_pill(notification_label_);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    style_status_pill(status_label_);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->background_color(), 0);

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    InitializeEmotionUi(screen, lvgl_theme, large_icon_font);
    EnsureMachineControlUi();
    if (provisioning_qr_root_ != nullptr) {
        lv_obj_move_foreground(provisioning_qr_root_);
    }
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    if (grobot_eyes_ != nullptr) {
        DisplayLockGuard lock(this);
        SetGrobotSubtitle(content);
        return;
    }
#endif
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    SetGrobotSubtitle("");
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Grobot 全脸没有独立 AI logo；其它 LVGL 聊天界面清屏后才恢复 logo。
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    if (grobot_eyes_ == nullptr && emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#else
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: Grobot face or emoji fallback, centered on the screen. */
    InitializeEmotionUi(screen, lvgl_theme, large_icon_font);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    EnsureMachineControlUi();
    if (provisioning_qr_root_ != nullptr) {
        lv_obj_move_foreground(provisioning_qr_root_);
    }
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    if (grobot_eyes_ != nullptr) {
        DisplayLockGuard lock(this);
        SetGrobotSubtitle(content);
        return;
    }
#endif
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    SetGrobotSubtitle("");
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif
void LcdDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    // 说话/聆听状态驱动全脸：说话嘴部开合+微弹跳、聆听瞳孔放大。
    // 状态写入与 33ms LVGL timer 读取必须在同一显示锁内。
    if (grobot_eyes_ != nullptr && status != nullptr) {
        DisplayLockGuard lock(this);
        grobot_eyes_->SetSpeaking(std::strcmp(status, Lang::Strings::SPEAKING) == 0);
        grobot_eyes_->SetListening(std::strcmp(status, Lang::Strings::LISTENING) == 0);
    }
#endif
}

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    if (grobot_eyes_) {
        DisplayLockGuard lock(this);
        grobot_eyes_->SetEmotion(emotion);
        return;
    }
#endif
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = nullptr;
        const lv_font_t* emotion_font = lvgl_theme->large_icon_font()->font();
        if (emoji_collection != nullptr) {
            utf8 = noto_emoji_get_utf8(emotion);
            emotion_font = lvgl_theme->emoji_font()->font();
        }
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    // 顶部图标不再占一整条半透明栏，只保留安静浮层。
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(status_label_, lvgl_theme->surface_color(), 0);
    lv_obj_set_style_bg_color(notification_label_, lvgl_theme->surface_color(), 0);
    lv_obj_set_style_border_color(status_label_, lvgl_theme->border_color(), 0);
    lv_obj_set_style_border_color(notification_label_, lvgl_theme->border_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    if (grobot_stage_ != nullptr) {
        lv_obj_set_style_bg_color(grobot_stage_, lvgl_theme->surface_color(), 0);
        lv_obj_set_style_border_color(grobot_stage_, lvgl_theme->border_color(), 0);
    }
    if (grobot_subtitle_bar_ != nullptr) {
        lv_obj_set_style_bg_color(grobot_subtitle_bar_, lvgl_theme->surface_color(), 0);
        lv_obj_set_style_border_color(grobot_subtitle_bar_, lvgl_theme->border_color(), 0);
        lv_obj_set_style_text_color(grobot_subtitle_label_, lvgl_theme->text_color(), 0);
    }

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif
    if (grobot_subtitle_bar_ != nullptr) {
        lv_obj_set_style_bg_color(grobot_subtitle_bar_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_bg_opa(grobot_subtitle_bar_, LV_OPA_80, 0);
    }
    if (grobot_subtitle_label_ != nullptr) {
        lv_obj_set_style_text_color(grobot_subtitle_label_, lvgl_theme->text_color(), 0);
    }

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (grobot_subtitle_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            const char* current = grobot_subtitle_label_ != nullptr
                                      ? lv_label_get_text(grobot_subtitle_label_)
                                      : nullptr;
            const std::string text = current != nullptr ? current : "";
            SetGrobotSubtitle(text.c_str());
        }
    }
}
