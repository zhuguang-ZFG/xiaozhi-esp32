import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CORE_HEADER = ROOT / "main" / "boards" / "lichuang-dev" / "hutuji_pi_splash_core.h"


def find_compiler():
    configured = os.environ.get("CXX", "").strip()
    candidates = [
        Path(configured) if configured else None,
        Path(found) if (found := shutil.which("clang++")) else None,
        Path("C:/Program Files/LLVM/bin/clang++.exe"),
        Path(found) if (found := shutil.which("g++")) else None,
    ]
    mingw_root = Path.home() / "scoop" / "apps" / "mingw"
    if mingw_root.is_dir():
        candidates.extend(sorted(mingw_root.glob("*/bin/g++.exe"), reverse=True))
    return next((path.resolve() for path in candidates if path and path.is_file()), None)


class HutujiPiSplashCoreTest(unittest.TestCase):
    def _compile_and_run(self, compiler, source, stem):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            source_path = temp / f"{stem}.cpp"
            output_path = temp / (f"{stem}.exe" if os.name == "nt" else stem)
            source_path.write_text(source, encoding="utf-8")
            build = subprocess.run(
                [
                    str(compiler),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(CORE_HEADER.parent),
                    str(source_path),
                    "-o",
                    str(output_path),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(output_path)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return run.stdout

    def setUp(self):
        self.compiler = find_compiler()
        if self.compiler is None:
            self.skipTest("no host C++ compiler available")

    def test_upstream_constants_are_verbatim(self):
        """字形/色标/动画常量必须与 omp 上游 welcome.ts 逐字一致。

        上游是「一比一复刻」的事实源；任何改动都会让板上动画与 omp 不同，
        必须由本测试挡住（尤其是有人为了开机快而缩短 3000ms）。
        """
        text = CORE_HEADER.read_text(encoding="utf-8")
        # PI_LOGO：5 行 × 12 列，ASCII 记号对应 ▀█╘▄。
        self.assertIn('"UFFFFFFFFFFU"', text)
        self.assertIn('" CFF    FF  "', text)
        self.assertIn('"  FF    FF  "', text)
        self.assertIn('" LFFL  LFFL "', text)
        self.assertIn("kPiLogoRowCount = 5", text)
        self.assertIn("kPiLogoColCount = 12", text)
        # GRADIENT_STOPS：热粉→紫罗兰→长春花→亮青→薄荷。
        for stop in ("{255, 92, 200}", "{200, 110, 255}", "{120, 130, 255}",
                     "{60, 200, 255}", "{120, 255, 220}"):
            self.assertIn(stop, text)
        # 动画参数原值。
        self.assertIn("kPiIntroMs = 3000", text)
        self.assertIn("kPiIntroTickMs = 33", text)
        self.assertIn("kPiIntroSweeps = 2.5f", text)
        self.assertIn("kPiIntroShineTraversals = 3.0f", text)
        self.assertIn("kPiShineHalfWidth = 0.18f", text)

    def test_span_matches_upstream_formula(self):
        """span = cols + rows - 1；上游靠这个 +1 效应让 base 严格 < 1。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    // span 与最大 base：base 必须严格小于 1，否则远角回绕到 t=0（热粉）。
    const float max_base = (float)(kPiLogoColCount - 1 + kPiLogoRowCount - 1) / (float)kPiLogoSpan;
    printf("span=%d max_base=%.6f\\n", kPiLogoSpan, max_base);
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_span")
        self.assertIn("span=16", out)
        max_base = float(out.split("max_base=")[1].split()[0])
        self.assertLess(max_base, 1.0)

    def test_intro_settles_on_rest_frame(self):
        """progress→1 时相位归 0、高光归 0，即落在静止帧；起点高光满强度。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    const PiIntroFrame start = PiIntroFrameAt(0.0f);
    const PiIntroFrame end = PiIntroFrameAt(1.0f);
    printf("start_shine=%.4f start_phase=%.4f\\n", start.shine_strength, start.phase);
    printf("end_shine=%.6f end_phase=%.6f\\n", end.shine_strength, end.phase);
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_intro")
        start_shine = float(out.split("start_shine=")[1].split()[0])
        start_phase = float(out.split("start_phase=")[1].split()[0])
        end_shine = float(out.split("end_shine=")[1].split()[0])
        end_phase = float(out.split("end_phase=")[1].split()[0])
        self.assertAlmostEqual(start_shine, 1.0, places=4)
        # 2.5 圈倒转：起点相位为 0.5（2.5 % 1），不是 0。
        self.assertAlmostEqual(start_phase, 0.5, places=4)
        self.assertLess(end_shine, 1e-5)
        self.assertLess(end_phase, 1e-5)

    def test_gradient_endpoints_are_exact_stops(self):
        """t=0 / t=1 必须精确落在首末色标，中点落在长春花。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    uint8_t r = 0, g = 0, b = 0;
    PiGradientRgb(0.0f, 0.0f, 0.0f, &r, &g, &b);
    printf("t0=%u,%u,%u\\n", r, g, b);
    PiGradientRgb(1.0f, 0.0f, 0.0f, &r, &g, &b);
    printf("t1=%u,%u,%u\\n", r, g, b);
    PiGradientRgb(0.5f, 0.0f, 0.0f, &r, &g, &b);
    printf("mid=%u,%u,%u\\n", r, g, b);
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_grad")
        self.assertIn("t0=255,92,200", out)
        self.assertIn("t1=120,255,220", out)
        self.assertIn("mid=120,130,255", out)

    def test_shine_peak_whitens_and_band_is_bounded(self):
        """高光峰值把颜色推到纯白；带外（距离 > 半宽）完全不受影响。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    uint8_t r = 0, g = 0, b = 0;
    // 峰值：t == shine_pos 且强度 1 → intensity 1 → 纯白。
    PiGradientRgb(0.5f, 1.0f, 0.5f, &r, &g, &b);
    printf("peak=%u,%u,%u\\n", r, g, b);
    // 带外：距离 0.5 > 半宽 0.18 → 与无高光一致。
    PiGradientRgb(0.0f, 1.0f, 0.5f, &r, &g, &b);
    printf("outside=%u,%u,%u\\n", r, g, b);
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_shine")
        self.assertIn("peak=255,255,255", out)
        self.assertIn("outside=255,92,200", out)

    def test_cell_rects_cover_expected_areas(self):
        """▀/▄ 各占半格、█ 占满格、╘ 为三段细笔；空白不产生矩形。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
static float Area(char code) {
    PiCellRect rects[kPiCellRectMax];
    const int n = PiCellRects(code, rects);
    float area = 0.0f;
    for (int i = 0; i < n; i++)
        area += (rects[i].x1 - rects[i].x0) * (rects[i].y1 - rects[i].y0);
    return area;
}
int main() {
    PiCellRect rects[kPiCellRectMax];
    printf("full=%d,%.4f\\n", PiCellRects('F', rects), Area('F'));
    printf("upper=%d,%.4f\\n", PiCellRects('U', rects), Area('U'));
    printf("lower=%d,%.4f\\n", PiCellRects('L', rects), Area('L'));
    printf("corner=%d\\n", PiCellRects('C', rects));
    printf("blank=%d\\n", PiCellRects(' ', rects));
    // ▀ 贴上边、▄ 贴下边，方向不能反（反了 π 顶横杠会掉到格子下半）。
    PiCellRects('U', rects);
    printf("upper_y=%.2f,%.2f\\n", rects[0].y0, rects[0].y1);
    PiCellRects('L', rects);
    printf("lower_y=%.2f,%.2f\\n", rects[0].y0, rects[0].y1);
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_cells")
        self.assertIn("full=1,1.0000", out)
        self.assertIn("upper=1,0.5000", out)
        self.assertIn("lower=1,0.5000", out)
        self.assertIn("corner=3", out)
        self.assertIn("blank=0", out)
        self.assertIn("upper_y=0.00,0.50", out)
        self.assertIn("lower_y=0.50,1.00", out)
        # ╘ 是细笔画，总面积远小于半格，否则会糊成方块。
        corner_area = None
        source_area = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    PiCellRect rects[kPiCellRectMax];
    const int n = PiCellRects('C', rects);
    float area = 0.0f;
    for (int i = 0; i < n; i++)
        area += (rects[i].x1 - rects[i].x0) * (rects[i].y1 - rects[i].y0);
    printf("area=%.4f\\n", area);
    return 0;
}
"""
        out_area = self._compile_and_run(self.compiler, source_area, "pi_corner_area")
        corner_area = float(out_area.split("area=")[1].split()[0])
        self.assertLess(corner_area, 0.25)
        self.assertGreater(corner_area, 0.0)

    def test_wrap01_normalizes_negative_and_over_one(self):
        """相位取模必须落在 [0,1)，负值与超 1 都要绕回。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    printf("neg=%.4f over=%.4f one=%.4f\\n", PiWrap01(-0.25f), PiWrap01(2.75f), PiWrap01(1.0f));
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_wrap")
        self.assertIn("neg=0.7500", out)
        self.assertIn("over=0.7500", out)
        self.assertIn("one=0.0000", out)

    def test_gradient_direction_is_bottom_left_to_top_right(self):
        """对角线方向：左下角 t 最小、右上角 t 最大（phase=0）。"""
        source = """
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    const float bottom_left = PiCellGradientT(0, kPiLogoRowCount - 1, 0.0f);
    const float top_right = PiCellGradientT(kPiLogoColCount - 1, 0, 0.0f);
    printf("bl=%.6f tr=%.6f\\n", bottom_left, top_right);
    return 0;
}
"""
        out = self._compile_and_run(self.compiler, source, "pi_dir")
        bl = float(out.split("bl=")[1].split()[0])
        tr = float(out.split("tr=")[1].split()[0])
        self.assertAlmostEqual(bl, 0.0, places=6)
        self.assertGreater(tr, bl)
        self.assertLess(tr, 1.0)

    def test_gradient_hex_matches_rgb_and_all_mood_positions(self):
        """Grobot 的 21 个情绪只能从 π 渐变取色；Hex helper 必须与 RGB 逐点一致。"""
        source = r'''
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    const float ts[] = {0.02f, 0.04f, 0.06f, 0.10f, 0.16f, 0.22f, 0.32f,
                        0.36f, 0.44f, 0.50f, 0.62f, 0.66f, 0.70f, 0.76f,
                        0.80f, 0.84f, 0.90f, 0.94f, 0.96f, 0.99f, 1.00f};
    for (float t : ts) {
        uint8_t r = 0, g = 0, b = 0;
        PiGradientRgb(t, 0.0f, 0.0f, &r, &g, &b);
        const uint32_t expected = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        if (PiGradientHex(t) != expected) return 10;
    }
    printf("brand=%06X sad=%06X crying=%06X angry=%06X happy=%06X\n",
           (unsigned)PiGradientHex(kPiBrandGradientT), (unsigned)PiGradientHex(0.62f),
           (unsigned)PiGradientHex(0.70f), (unsigned)PiGradientHex(0.02f),
           (unsigned)PiGradientHex(0.99f));
    return 0;
}
'''
        out = self._compile_and_run(self.compiler, source, "pi_mood_palette")
        # 主 UI/Grobot 锚在 π 静止帧占比最高的中段蓝紫；负面情绪保持可辨层次。
        self.assertIn("brand=7882FF", out)
        self.assertIn("sad=5BA4FF", out)
        self.assertIn("crying=48BAFF", out)
        self.assertIn("angry=FB5DCC", out)
        self.assertIn("happy=76FDDD", out)

    def test_pixel_rounding_is_shared_and_contiguous(self):
        """单元边独立取整；相邻整格不能出现缝隙/重叠，╘ 的 1/8 笔画可复现。"""
        source = r'''
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    PiCellRect full[3];
    PiCellRects('F', full);
    const PiPixelRect a = PiRectToPixels(full[0], 0, 0, 32, 48);
    const PiPixelRect b = PiRectToPixels(full[0], 32, 0, 32, 48);
    PiCellRect corner[3];
    PiCellRects('C', corner);
    const PiPixelRect stem = PiRectToPixels(corner[0], 0, 0, 32, 48);
    printf("a=%d,%d,%d,%d b=%d,%d,%d,%d stem=%d,%d,%d,%d\n",
           a.x0,a.y0,a.x1,a.y1,b.x0,b.y0,b.x1,b.y1,
           stem.x0,stem.y0,stem.x1,stem.y1);
    return (a.x1 == b.x0 && a.x1 == 32) ? 0 : 20;
}
'''
        out = self._compile_and_run(self.compiler, source, "pi_pixels")
        self.assertIn("a=0,0,32,48 b=32,0,64,48", out)
        self.assertIn("stem=14,0,18,33", out)

    def test_face_gradient_full_sweep_rides_logo_diagonal(self):
        """Grobot 全脸与 logo 静止帧同一条 0..1 全程渐变：左下热粉、右上薄荷。
        span 用 +1 技巧保证 base 严格 < 1（绕回缝不落在画布内）；相位绕回正常。"""
        text = CORE_HEADER.read_text(encoding="utf-8")
        self.assertIn("kPiFacePhaseSwing = 0.30f", text)
        source = r'''
#include <cstdio>
#include "hutuji_pi_splash_core.h"
int main() {
    // 480x320 画布，phase=0 必须与 logo 静止帧一致：左下 t=0、右上 t≈1 但严格 <1。
    const float bl = PiFaceGradientT(0, 319, 480, 320, 0.0f);
    const float tr = PiFaceGradientT(479, 0, 480, 320, 0.0f);
    // 相位绕回：phase=0.5 时左下 0.5、右上 wrap(1.4987)=0.4987。
    const float pbl = PiFaceGradientT(0, 319, 480, 320, 0.50f);
    const float ptr = PiFaceGradientT(479, 0, 480, 320, 0.50f);
    // 两端必须是 logo 的首末色标：热粉与薄荷。
    uint8_t r0 = 0, g0 = 0, b0 = 0, r1 = 0, g1 = 0, b1 = 0;
    PiGradientRgb(bl, 0, 0, &r0, &g0, &b0);
    PiGradientRgb(tr, 0, 0, &r1, &g1, &b1);
    printf("bl=%.4f tr=%.4f pbl=%.4f ptr=%.4f end0=%02X%02X%02X end1=%02X%02X%02X\n",
           bl, tr, pbl, ptr, (int)r0, (int)g0, (int)b0, (int)r1, (int)g1, (int)b1);
    return 0;
}
'''
        out = self._compile_and_run(self.compiler, source, "pi_face_gradient")
        self.assertIn("bl=0.0000", out)
        self.assertIn("tr=0.9987", out)  # (w+h-2)/(w+h-1)，严格 < 1
        self.assertIn("pbl=0.5000", out)
        self.assertIn("ptr=0.4987", out)  # wrap(0.9987 + 0.5)
        self.assertIn("end0=FF5CC8", out)  # t=0：热粉（logo 首色标）
        self.assertIn("end1=78FFDC", out)  # t≈1：薄荷（logo 末色标）


if __name__ == "__main__":
    unittest.main()
