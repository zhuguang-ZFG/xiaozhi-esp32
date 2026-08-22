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
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5 && CONFIG_HUTUJI_GROBOT_FACE
class HutujiPiSplash;
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
    // 实体 boot 键功能上屏（2026-08-20 商业化少按键决策）：说话 = boot 单击
    // （ToggleChatState，starting 态转配网），配网 = 直接进配网模式显二维码。
    lv_obj_t* voice_talk_btn_ = nullptr;
    lv_obj_t* wifi_config_btn_ = nullptr;
    std::function<void()> voice_talk_;
    std::function<void()> wifi_config_;
    // 配网二维码层的「关闭」：仅在板级注册过取消回调时显示（其他板不受影响）。
    lv_obj_t* provisioning_cancel_btn_ = nullptr;
    std::function<void()> provisioning_on_cancel_;
    // 与触发钮同款的按下跟随拖动状态（24px 阈值区分点按/拖动）。
    struct HomeButtonDrag {
        lv_point_t press_point{0, 0};
        lv_coord_t press_x = 0;
        lv_coord_t press_y = 0;
        bool dragging = false;
        std::function<void()>* action = nullptr;  // 非拥有指针，指向 LcdDisplay 成员的槽位
        const char* nvs_prefix = nullptr;         // NVS「hutuji_ui」命名空间下的坐标键前缀
    };
    HomeButtonDrag voice_talk_drag_;
    HomeButtonDrag wifi_config_drag_;
    /** 给主页入口钮挂「按下跟随 + 24px 阈值 + 松手未拖才触发 + 落点写 NVS」行为。 */
    void AttachHomeEntryButton(lv_obj_t* btn, HomeButtonDrag* state, std::function<void()>* action,
                               const char* nvs_prefix);
    /** 布局记忆：写/读 NVS「hutuji_ui」中的按钮坐标；Load 越界返回 false。 */
    static void SaveHomeButtonPos(const char* prefix, lv_coord_t x, lv_coord_t y);
    static bool LoadHomeButtonPos(const char* prefix, lv_coord_t* x, lv_coord_t* y);
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
    // 抽屉分页：主操作区与手动区互斥显示，各自都塞得进 296px 面板内高。面板不可
    // 滚（height 固定，见 lcd_display.cc EnsureMachineControlUi 注释），同屏堆叠会
    // 把按钮挤出可视区——2026-08-20 四轮 HIL 实测丢按钮的根因。
    lv_obj_t* machine_main_section_ = nullptr;
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
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5 && CONFIG_HUTUJI_GROBOT_FACE
    // 开机 π logo 启动画面；播完自行拆掉所有 LVGL 对象，仅析构时兜底。
    std::unique_ptr<HutujiPiSplash> pi_splash_;
#endif
#if CONFIG_HUTUJI_GROBOT_FACE
    // 主屏 accent 按钮呼吸定时器：π 品牌中段 ±0.06 漂移；安全语义色不参与。
    lv_timer_t* accent_drift_timer_ = nullptr;
    static void AccentDriftTimerCb(lv_timer_t* timer);
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
    void ShowDrawPreviewLoading() override;
    virtual void HideDrawPreview() override;
    void ConfigureMachineControls(std::function<void()> on_pause, std::function<void()> on_resume,
                                  std::function<void()> on_abort, std::function<void()> on_repeat,
                                  std::function<void()> on_pen_test,
                                  std::function<void(const char* action)> on_manual);
    /** boot 键功能上屏：on_talk = boot 单击等效，on_wifi = 进配网显二维码。 */
    void ConfigureVoiceEntry(std::function<void()> on_talk, std::function<void()> on_wifi);
    /** 配网二维码「关闭」回调：调用方负责退出配网模式（如 StopConfigAp）。 */
    void SetProvisioningCancelHandler(std::function<void()> on_cancel);
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
