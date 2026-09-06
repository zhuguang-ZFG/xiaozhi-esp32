#pragma once

#include <cstdint>
#include <string>

#include "display/lvgl_display/emoji_collection.h"

// ElectronBot 桌宠脸（2026-09-06 用户拍板替换 grobot 程序绘眼）。
// 资产：peng-zhihui/ElectronBot@81992701 4.CAD-Model/Emoji mp4 → 240x240@12fps 灰度 GIF
// （嵌入固件 electronbot_face_assets.h，合计 180KB；用户豁免许可证，出货前须复核——
// 上游仓整体 GPLv3，详见 hutuji 仓 docs/design/face-electronbot-0906.md）。
//
// 设计：LcdDisplay 的 SetEmotion GIF 路径原样复用，本类只负责「21 个 xiaozhi 情绪名 →
// 6 组 ElectronBot 表情」的别名映射与 neutral 兜底（GetEmojiImage 永不落空，不会退化到
// 图标字体）。眨眼偶发由 LcdDisplay 的定时器驱动（blink_once/blink_twice 轮换）。

class ElectronBotEmojiCollection : public EmojiCollection {
public:
    ElectronBotEmojiCollection();  // 注册全部内嵌资产（canonical 键 + 21 名别名）
    const LvglImage* GetEmojiImage(const char* name) override;

    // canonical 键：happy/sad/angry/shocked/disdain/neutral/blink_once/blink_twice
    static const char* MapEmotion(const char* name);  // 返回 canonical 键（未知→neutral）

private:
    void AddRaw(const std::string& key, const uint8_t* data, size_t size);
};
