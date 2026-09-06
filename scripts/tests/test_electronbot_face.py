"""ElectronBot 桌宠脸（2026-09-06 用户拍板替掉 grobot 程序绘眼）源码钉。

不动 LVGL 编译；钉住接线面：CMake 开关、SetupUI 分支、SetEmotion 映射与恢复、
暂停钩子双路径、眨眼恢复守卫、情绪名映射对 grobot 21 名的完整覆盖、资产头符号。
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LCD_CC = ROOT / "main/display/lcd_display.cc"
LCD_H = ROOT / "main/display/lcd_display.h"
CMAKE = ROOT / "main/CMakeLists.txt"
FACE_CC = ROOT / "main/boards/lichuang-dev/electronbot_face.cc"
FACE_H = ROOT / "main/boards/lichuang-dev/electronbot_face.h"
ASSETS_H = ROOT / "main/boards/lichuang-dev/electronbot_face_assets.h"
GROBOT_CC = ROOT / "main/boards/lichuang-dev/grobot_eyes.cc"


class ElectronBotFaceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lcd_cc = LCD_CC.read_text(encoding="utf-8")
        cls.lcd_h = LCD_H.read_text(encoding="utf-8")
        cls.cmake = CMAKE.read_text(encoding="utf-8")
        cls.face_cc = FACE_CC.read_text(encoding="utf-8")
        cls.assets_h = ASSETS_H.read_text(encoding="utf-8")
        cls.grobot_cc = GROBOT_CC.read_text(encoding="utf-8")

    def test_setupui_electronbot_branch_replaces_eyes(self):
        # 分支内：electronbot 建集合脸 + 共用字幕条；grobot 建眼睛画布
        self.assertIn('#if CONFIG_HUTUJI_ELECTRONBOT_FACE', self.lcd_cc)
        self.assertIn('InitElectronBotFace(theme);', self.lcd_cc)
        self.assertIn('CreateGrobotSubtitleBar(screen, theme);', self.lcd_cc)
        # 字幕条抽公共方法后两分支共用（不在 eyes->Init 成功分支里独享）
        self.assertIn('void LcdDisplay::CreateGrobotSubtitleBar', self.lcd_cc)

    def test_cmake_option_scoped_to_waveshare_and_wired(self):
        # 默认开只在 waveshare-3.5 板分支（CACHE BOOL 不带 FORCE，-D=OFF 可回退 grobot）
        m = re.search(r'elseif\(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5\)(.*?)elseif\(',
                      self.cmake, re.S)
        self.assertIsNotNone(m)
        self.assertIn('set(HUTUJI_ELECTRONBOT_FACE ON CACHE BOOL', m.group(1))
        # 全局无 option() 默认开——lichuang-dev 等其他板不被顺手换脸
        self.assertNotIn('option(HUTUJI_ELECTRONBOT_FACE', self.cmake)
        self.assertIn('CONFIG_HUTUJI_ELECTRONBOT_FACE=1', self.cmake)
        self.assertIn('boards/lichuang-dev/electronbot_face.cc', self.cmake)

    def test_setemotion_electronbot_records_mapped_emotion(self):
        # SetEmotion 的 electronbot 分支：归一化记录 + 定向播放 + return
        m = re.search(
            r'if \(electronbot_face_active_\) \{\s*DisplayLockGuard lock\(this\);[^}]*?'
            r'electronbot_emotion_ = ElectronBotEmojiCollection::MapEmotion\(emotion\);[^}]*?'
            r'ElectronBotShow\(electronbot_emotion_\.c_str\(\)\);\s*return;',
            self.lcd_cc, re.S)
        self.assertIsNotNone(m, "SetEmotion 缺 electronbot 映射+播放分支")

    def test_pause_hook_freezes_and_replays(self):
        m = re.search(r'void LcdDisplay::SetGrobotEyesPaused\(bool on\) \{(.*?)\n\}',
                      self.lcd_cc, re.S)
        self.assertIsNotNone(m)
        body = m.group(1)
        self.assertIn('electronbot_face_active_', body)
        self.assertIn('electronbot_paused_ = on;', body)
        self.assertIn('gif_controller_->Stop();', body)
        # 恢复重放当前情绪（不停在停帧上卡死）
        self.assertIn('ElectronBotShow(electronbot_emotion_.c_str());', body)
        # grobot 路径保留
        self.assertIn('grobot_eyes_->SetPaused(on);', body)

    def test_blink_timer_and_restore_guard(self):
        self.assertIn('lv_timer_create(ElectronBotBlinkTimerCb, 5000, this)', self.lcd_cc)
        self.assertIn('lv_timer_set_repeat_count(restore, 1)', self.lcd_cc)
        # 恢复帧只在仍是 neutral 时才回，不盖真情绪
        m = re.search(r'void LcdDisplay::ElectronBotBlinkRestoreCb.*?'
                      r'electronbot_emotion_ == "neutral"', self.lcd_cc, re.S)
        self.assertIsNotNone(m)
        # 眨眼途中暂停/真情绪到达都被拦
        self.assertIn('electronbot_paused_', self.lcd_cc)

    def test_theme_switch_reattaches_collection(self):
        m = re.search(r'void LcdDisplay::SetTheme\(Theme\* theme\) \{(.*?)\n\}',
                      self.lcd_cc, re.S)
        self.assertIsNotNone(m)
        self.assertIn('set_emoji_collection(electronbot_collection_)', m.group(1))

    def test_emotion_mapping_covers_all_grobot_names(self):
        m = re.search(r'kNames\[\] = \{(.*?)\};', self.grobot_cc, re.S)
        self.assertIsNotNone(m)
        grobot_names = re.findall(r'"(\w+)"', m.group(1))
        self.assertGreaterEqual(len(grobot_names), 20)
        mapped = set(re.findall(r'"(\w+)"', self.face_cc))
        missing = [n for n in grobot_names if n not in mapped]
        self.assertEqual(missing, [], f"情绪名未映射: {missing}")
        # canonical 键与眨眼键都在
        for key in ("happy", "sad", "angry", "shocked", "disdain", "neutral",
                    "blink_once", "blink_twice"):
            self.assertIn(f'"{key}"', self.face_cc)

    def test_assets_header_symbols_and_nonempty(self):
        for sym in ("kFaceHappyLoop", "kFaceSadLoop", "kFaceAngryLoop",
                    "kFaceShockedLoop", "kFaceDisdainLoop", "kFaceBlinkOnce",
                    "kFaceBlinkTwice", "kFaceNeutralStatic"):
            self.assertIn(sym, self.assets_h)
            m = re.search(rf'{sym}Size = (\d+);', self.assets_h)
            self.assertIsNotNone(m, sym)
            self.assertGreater(int(m.group(1)), 1000, sym)  # 非空资产
        # GetEmojiImage 兜底 neutral（未知情绪名不退化到图标字体）
        self.assertIn('EmojiCollection::GetEmojiImage(MapEmotion(name))', self.face_cc)


if __name__ == "__main__":
    unittest.main()
