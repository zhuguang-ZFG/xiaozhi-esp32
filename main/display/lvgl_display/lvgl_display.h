#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

class DynamicGlyphCache;
class LvglFont;

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    // 出图预览确认层：全屏覆盖，与聊天气泡/全脸情绪层无关，故不复用 SetPreviewImage
    // （后者在 WECHAT 风格下是会话气泡且忽略 nullptr，无法承载"确认前不出图"的门）。
    // on_confirm/on_cancel 在 LVGL 任务里被调用，实现必须只做转发、不阻塞。
    virtual void ShowDrawPreview(std::unique_ptr<LvglImage> image, const std::string& hint,
                                 std::function<void()> on_confirm,
                                 std::function<void()> on_cancel);
    virtual void HideDrawPreview();
    virtual void UpdateMachineControlState(const std::string& state);

    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
    virtual void ClearTextGlyphs() override;
    bool SetTextFont(std::shared_ptr<LvglFont> text_font);

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t* display_ = nullptr;

    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;

    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;
    std::unique_ptr<DynamicGlyphCache> dynamic_glyph_cache_;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

#endif
