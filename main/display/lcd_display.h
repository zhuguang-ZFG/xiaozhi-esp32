#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>
#include <vector>

#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
class GrobotEyes;
#endif

#define PREVIEW_IMAGE_DURATION_MS 5000

class LvglTheme;

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* grobot_stage_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* grobot_subtitle_bar_ = nullptr;
    lv_obj_t* grobot_subtitle_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    lv_obj_t* provisioning_qr_root_ = nullptr;
    lv_obj_t* provisioning_qr_code_ = nullptr;
    lv_obj_t* provisioning_qr_hint_ = nullptr;
    std::unique_ptr<LvglImage> provisioning_qr_image_;
    lv_obj_t* draw_preview_root_ = nullptr;
    lv_obj_t* draw_preview_card_ = nullptr;
    lv_obj_t* draw_preview_image_ = nullptr;
    lv_obj_t* draw_preview_hint_ = nullptr;
    lv_obj_t* draw_preview_confirm_btn_ = nullptr;
    lv_obj_t* draw_preview_cancel_btn_ = nullptr;
    std::unique_ptr<LvglImage> draw_preview_cached_;
    std::function<void()> draw_preview_on_confirm_;
    std::function<void()> draw_preview_on_cancel_;
    lv_obj_t* machine_control_trigger_btn_ = nullptr;
    lv_point_t machine_trigger_press_point_{0, 0};
    lv_coord_t machine_trigger_press_x_ = 0;
    lv_coord_t machine_trigger_press_y_ = 0;
    bool machine_trigger_dragging_ = false;
    lv_obj_t* machine_control_root_ = nullptr;
    lv_obj_t* machine_pause_btn_ = nullptr;
    lv_obj_t* machine_resume_btn_ = nullptr;
    lv_obj_t* machine_abort_btn_ = nullptr;
    lv_obj_t* machine_repeat_btn_ = nullptr;
    lv_obj_t* machine_pen_test_btn_ = nullptr;
    lv_obj_t* machine_close_btn_ = nullptr;
    lv_obj_t* machine_state_label_ = nullptr;
    lv_obj_t* machine_manual_section_ = nullptr;
    lv_obj_t* machine_manual_toggle_btn_ = nullptr;
    lv_obj_t* machine_manual_toggle_label_ = nullptr;
    std::function<void()> machine_pause_;
    std::function<void()> machine_resume_;
    std::function<void()> machine_abort_;
    std::function<void()> machine_repeat_;
    std::function<void()> machine_pen_test_;
    std::function<void(const char* action)> machine_manual_;
    std::vector<lv_obj_t*> machine_manual_buttons_;
    std::string machine_state_{"idle"};
    bool machine_controls_configured_ = false;
#if CONFIG_BOARD_TYPE_LICHUANG_DEV_S3 || CONFIG_HUTUJI_GROBOT_FACE
    std::unique_ptr<GrobotEyes> grobot_eyes_;
#endif

    void InitializeLcdThemes();
    void EnsureProvisioningQrUi();
    void EnsureDrawPreviewUi();
    void EnsureMachineControlUi();
    void ApplyMachineControlState();
    void SetGrobotSubtitle(const char* content);
    void SetMachineManualSectionVisible(bool visible);
    void InitializeEmotionUi(lv_obj_t* screen, LvglTheme* theme, const lv_font_t* large_icon_font);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void ShowProvisioningQr(const std::string& payload, const std::string& hint) override;
    virtual void HideProvisioningQr() override;
    virtual void ShowDrawPreview(std::unique_ptr<LvglImage> image, const std::string& hint,
                                 std::function<void()> on_confirm,
                                 std::function<void()> on_cancel) override;
    virtual void HideDrawPreview() override;
    void ConfigureMachineControls(std::function<void()> on_pause, std::function<void()> on_resume,
                                  std::function<void()> on_abort, std::function<void()> on_repeat,
                                  std::function<void()> on_pen_test,
                                  std::function<void(const char* action)> on_manual);
    virtual void UpdateMachineControlState(const std::string& state) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
