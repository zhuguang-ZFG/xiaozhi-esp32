import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


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


class HutujiMusicCoreTest(unittest.TestCase):
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
                    str(ROOT),
                    str(source_path),
                    "-o",
                    str(output_path),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(build.returncode, 0, build.stderr or build.stdout)
            env = os.environ.copy()
            env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            run = subprocess.run(
                [str(output_path)],
                cwd=temp,
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )
            self.assertEqual(run.returncode, 0, run.stderr or run.stdout)

    def test_song_url_whitelist(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_music_core.h"
            #include <cassert>

            int main() {
                using hutuji::music::IsValidSongUrl;
                // 生产域：HTTPS、/songs/、.ogg；裸域或 :443 都行
                assert(IsValidSongUrl("https://hutuji.donglicao.com/songs/twinkle.ogg"));
                assert(IsValidSongUrl("https://hutuji.donglicao.com:443/songs/twinkle.ogg"));
                assert(IsValidSongUrl("https://HUTUJI.donglicao.com/songs/a.ogg"));
                // RFC1918 联调主机允许 HTTP
                assert(IsValidSongUrl("http://192.168.1.13:8300/songs/twinkle.ogg"));
                assert(IsValidSongUrl("http://10.0.0.2/songs/x.ogg"));
                assert(IsValidSongUrl("http://172.16.0.9/songs/x.ogg"));
                // 生产域不允许 HTTP、不允许非 443 端口
                assert(!IsValidSongUrl("http://hutuji.donglicao.com/songs/twinkle.ogg"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com:8443/songs/twinkle.ogg"));
                // 路径必须 /songs/，后缀必须 .ogg；G-code/PNG 互换被拒
                assert(!IsValidSongUrl("https://hutuji.donglicao.com/files/draw_20260101_000000_abcdefghijklmnopqrstuv.gcode"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com/songs/twinkle.gcode"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com/songs/twinkle.oggx"));
                // authority 注入面：userinfo / IPv6 / 空白 / fragment / 畸形端口
                assert(!IsValidSongUrl("https://hutuji.donglicao.com@evil.com/songs/a.ogg"));
                assert(!IsValidSongUrl("https://[::1]/songs/a.ogg"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com /songs/a.ogg"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com/songs/a.ogg#x"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com:0/songs/a.ogg"));
                assert(!IsValidSongUrl("https://hutuji.donglicao.com:99999/songs/a.ogg"));
                // 非 RFC1918 数字主机不许 HTTP；query 不影响后缀判断
                assert(!IsValidSongUrl("http://8.8.8.8/songs/a.ogg"));
                assert(IsValidSongUrl("https://hutuji.donglicao.com/songs/a.ogg?v=1"));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_music_url_test")

    def test_music_state_transition_table_is_exact(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_music_core.h"
            #include <cassert>

            int main() {
                using hutuji::music::CanTransition;
                using hutuji::music::MusicState;
                // 全 4x4 枚举，合法集合逐一枚举，防止将来加状态时顺手放宽
                const bool expected[4][4] = {
                    // to:   idle   down   play   stop
                    /* idle */ {false, true,  false, false},
                    /* down */ {true,  false, true,  true},
                    /* play */ {true,  false, false, true},
                    /* stop */ {true,  false, false, false},
                };
                const MusicState states[4] = {MusicState::kIdle, MusicState::kDownloading,
                                              MusicState::kPlaying, MusicState::kStopping};
                for (int from = 0; from < 4; ++from) {
                    for (int to = 0; to < 4; ++to) {
                        assert(CanTransition(states[from], states[to]) == expected[from][to]);
                    }
                }
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_music_state_test")

    def test_music_constants_match_contract(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_music_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::music;
                static_assert(kMaxSongBytes == 4u * 1024u * 1024u);
                static_assert(kDemuxChunkBytes == 32u * 1024u);
                static_assert(kMaxTitleChars == 64u);
                // 下载上限与云端合成单曲（<200KB）之间留 20x 余量，防止未来
                // 加歌时因上限过小而悄悄拒绝
                static_assert(kMaxSongBytes >= 20u * 200u * 1024u);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_music_constants_test")


if __name__ == "__main__":
    unittest.main()
