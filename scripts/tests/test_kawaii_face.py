"""kawaii 桌宠脸（2026-09-06 用户拍板，同日替掉 ElectronBot GIF 脸）源码钉。

钉住接线面：CMake 板级默认开、SetupUI 分支、SetEmotion 映射与暂停期只记录、
暂停钩子 → face_animation_set_paused、vendored 补丁在位（暂停 API + SPIRAM 优先）、
21 情绪名全覆盖映射、ElectronBot 资产已拆除。
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LCD_CC = ROOT / "main/display/lcd_display.cc"
CMAKE = ROOT / "main/CMakeLists.txt"
KAWAII_C = ROOT / "main/boards/lichuang-dev/kawaii_face/lvgl_kawaii_face.c"
KAWAII_H = ROOT / "main/boards/lichuang-dev/kawaii_face/lvgl_kawaii_face.h"
GROBOT_CC = ROOT / "main/boards/lichuang-dev/grobot_eyes.cc"


class KawaiiFaceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lcd_cc = LCD_CC.read_text(encoding="utf-8")
        cls.cmake = CMAKE.read_text(encoding="utf-8")
        cls.kawaii_c = KAWAII_C.read_text(encoding="utf-8")
        cls.kawaii_h = KAWAII_H.read_text(encoding="utf-8")
        cls.grobot_cc = GROBOT_CC.read_text(encoding="utf-8")

    def test_cmake_scoped_default_on_waveshare_only(self):
        # 默认开只在 waveshare-3.5 板分支（CACHE BOOL 不带 FORCE，-D=OFF 回退 grobot）
        m = re.search(r'elseif\(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5\)(.*?)elseif\(',
                      self.cmake, re.S)
        self.assertIsNotNone(m)
        self.assertIn('set(HUTUJI_KAWAII_FACE ON CACHE BOOL', m.group(1))
        # 全局无 option() 默认开——lichuang-dev 等其他板不被顺手换脸
        self.assertNotIn('option(HUTUJI_KAWAII_FACE', self.cmake)
        self.assertIn('CONFIG_HUTUJI_KAWAII_FACE=1', self.cmake)
        self.assertIn('boards/lichuang-dev/kawaii_face/lvgl_kawaii_face.c', self.cmake)

    def test_setupui_kawaii_branch(self):
        self.assertIn('#if CONFIG_HUTUJI_KAWAII_FACE', self.lcd_cc)
        self.assertIn('InitKawaiiFace();', self.lcd_cc)
        self.assertIn('CreateGrobotSubtitleBar(screen, theme);', self.lcd_cc)
        # 初始化失败回落 emoji 图标路径，不拖垮 UI
        self.assertIn('kawaii_face_active_ = false;', self.lcd_cc)

    def test_setemotion_maps_and_respects_pause(self):
        # 记录归一化情绪；暂停期只记录不播放
        m = re.search(r'if \(kawaii_face_active_\) \{\s*DisplayLockGuard lock\(this\);[^;]*?'
                      r'kawaii_emotion_ = emotion != nullptr \? emotion : "neutral";\s*'
                      r'if \(!kawaii_paused_\) \{\s*'
                      r'face_set_emotion\(MapKawaiiEmotion_\(kawaii_emotion_\.c_str\(\)\), true\);',
                      self.lcd_cc, re.S)
        self.assertIsNotNone(m, "SetEmotion 缺 kawaii 映射/暂停守卫分支")

    def test_pause_hook_drives_animation_timer(self):
        m = re.search(r'void LcdDisplay::SetGrobotEyesPaused\(bool on\) \{(.*?)\n\}',
                      self.lcd_cc, re.S)
        self.assertIsNotNone(m)
        body = m.group(1)
        self.assertIn('kawaii_paused_ = on;', body)
        self.assertIn('face_animation_set_paused(on);', body)
        # 恢复时按最新记录情绪落一次（暂停期 SetEmotion 只记录）
        self.assertIn('face_set_emotion(MapKawaiiEmotion_(kawaii_emotion_.c_str()), true);', body)
        # grobot 路径保留
        self.assertIn('grobot_eyes_->SetPaused(on);', body)

    def test_vendor_patches_present(self):
        # 补丁 1：暂停 API（上游无）声明+实现
        self.assertIn('face_animation_set_paused', self.kawaii_h)
        self.assertIn('lv_timer_pause(face_state.anim_timer)', self.kawaii_c)
        self.assertIn('lv_timer_resume(face_state.anim_timer)', self.kawaii_c)
        # 补丁 2：画布分配 SPIRAM 优先（内部 SRAM 是 TLS/音频命脉）
        m = re.search(r'face_malloc_canvas\(size_t size\)\s*\{(.*?)return malloc', self.kawaii_c, re.S)
        self.assertIsNotNone(m)
        body = m.group(1)
        self.assertLess(body.index('MALLOC_CAP_SPIRAM'), body.index('MALLOC_CAP_INTERNAL'))
        # 补丁 3：Eilik 风（2026-09-06 用户拍板方案 1）
        self.assertIn('FACE_EILIK_EYE_RATIO_NUM 88', self.kawaii_c)
        self.assertIn('FACE_EILIK_IRIS_R 180', self.kawaii_c)
        self.assertIn('FACE_EILIK_IRIS_G 240', self.kawaii_c)
        self.assertIn('FACE_EILIK_IRIS_B 255', self.kawaii_c)
        self.assertIn('FACE_EILIK_BLUSH_DIV 5', self.kawaii_c)
        self.assertIn('FACE_EILIK_ACCENT_HEX 0xB4F0FF', self.kawaii_h)
        self.assertIn('0xB4F0FF', self.lcd_cc)
        # kawaii 开时主题 accent 必须走淡青分支，不能被 GROBOT π 蓝紫盖住
        self.assertIn('#if CONFIG_HUTUJI_KAWAII_FACE', self.lcd_cc)
        m = re.search(r'#if CONFIG_HUTUJI_KAWAII_FACE\s*(.*?)#elif CONFIG_HUTUJI_GROBOT_FACE',
                      self.lcd_cc, re.S)
        self.assertIsNotNone(m, "缺 KAWAII 优先于 GROBOT 的 accent 分支")
        self.assertIn('0xB4F0FF', m.group(1))
        self.assertIn('has_accent = true', self.lcd_cc)
        self.assertIn('theme->accent_color()', self.lcd_cc)
        # AccentDrift 在 kawaii 下必须读主题 accent，禁止每 100ms 刷回 π 蓝紫
        drift = self.lcd_cc[self.lcd_cc.index('void LcdDisplay::AccentDriftTimerCb'):
                            self.lcd_cc.index('void LcdDisplay::SetupUI')]
        self.assertIn('#if CONFIG_HUTUJI_KAWAII_FACE', drift)
        self.assertIn('theme->accent_color()', drift)
        self.assertIn('0xB4F0FF', drift)
        self.assertIn('lv_canvas_fill_bg(canvas, lv_color_black()', self.kawaii_c)
        self.assertIn('lv_obj_set_style_bg_color(face_state.face_container, lv_color_black()',
                      self.kawaii_c)
        # 铺满父盒（禁止回退 min 方块裁切）
        self.assertIn('lv_obj_set_size(face_state.face_container, parent_w, parent_h)',
                      self.kawaii_c)
        self.assertIn('parent_w * 0.46f', self.kawaii_c)
        # 禁止回退白底眼白框
        self.assertNotIn('lv_canvas_fill_bg(canvas, lv_color_white()', self.kawaii_c)
        self.assertNotIn('lv_color_make(50, 180, 255)', self.kawaii_c)

    def test_lichuang_face_box_nearly_fullscreen(self):
        # 立创 320x240：脸盒须近全屏（旧 280x190 目视留白被否）
        self.assertIn('constexpr int kFaceWidth = 312;', self.lcd_cc)
        self.assertIn('constexpr int kFaceHeight = 232;', self.lcd_cc)
        self.assertNotIn('constexpr int kFaceWidth = 280;', self.lcd_cc)
        self.assertNotIn('constexpr int kFaceHeight = 190;', self.lcd_cc)

    def test_emotion_mapping_covers_all_grobot_names(self):
        m = re.search(r'kNames\[\] = \{(.*?)\};', self.grobot_cc, re.S)
        self.assertIsNotNone(m)
        grobot_names = set(re.findall(r'"(\w+)"', m.group(1)))
        self.assertGreaterEqual(len(grobot_names), 20)
        # 只收 MapKawaiiEmotion_ 函数体内的映射组字面量（防注释里的词混进集合）
        region = self.lcd_cc[self.lcd_cc.index('face_emotion_t LcdDisplay::MapKawaiiEmotion_'):
                             self.lcd_cc.index('void LcdDisplay::InitKawaiiFace')]
        mapped = set(re.findall(r'"(\w+)"', region))
        missing = grobot_names - mapped
        self.assertEqual(missing, set(), f"情绪名未映射: {missing}")

    def test_electronbot_face_fully_removed(self):
        """ElectronBot GIF 脸（同日被实物否决）拆除干净，无残留引用。"""
        for token in ('ElectronBotShow', 'ElectronBotMakeCached_', 'electronbot_collection_',
                      'electronbot_face_assets', 'ElectronBotEmojiCollection'):
            self.assertNotIn(token, self.lcd_cc)
        for gone in ('electronbot_face.cc', 'electronbot_face.h', 'electronbot_face_assets.h'):
            self.assertFalse((ROOT / 'main/boards/lichuang-dev' / gone).exists(), gone)


if __name__ == "__main__":
    unittest.main()
