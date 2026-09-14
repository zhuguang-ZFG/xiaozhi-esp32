"""运行实际二维码编码/栅格/显隐函数；LVGL 几何另由真实渲染验收。"""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_hutuji_recovery_core import find_compiler

ROOT = Path(__file__).resolve().parents[2]
DISPLAY_SOURCE = ROOT / "main/display/lcd_display.cc"


def function_source(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError(signature)


class ProvisioningQrTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = find_compiler()
        if compiler is None:
            raise RuntimeError("二维码回归必须有 host C/C++ 编译器")
        cls.directory = tempfile.TemporaryDirectory(prefix="provisioning-qr-")
        cls.addClassCleanup(cls.directory.cleanup)
        temp = Path(cls.directory.name)
        cls.env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ.get("PATH", ""))
        (temp / "esp_err.h").write_text("""#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
""", encoding="utf-8")
        (temp / "esp_log.h").write_text("#pragma once\n#define ESP_LOGI(...) ((void)0)\n", encoding="utf-8")
        qr = ROOT / "components/hutuji_qrcode"
        objects = []
        # 同一份固件 C 编码器与 ESP 包装；不能用测试替身把长载荷伪装成成功。
        for name in ("qrcodegen", "esp_qrcode_main", "esp_qrcode_wrapper"):
            obj = temp / (name + ".o")
            cls.run_command([str(compiler), "-x", "c", "-std=c11", "-I", str(temp),
                             "-I", str(qr / "include"), "-c", str(qr / (name + ".c")), "-o", str(obj)])
            objects.append(str(obj))
        public = bytes.fromhex(
            "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
        )
        identity = [hashlib.sha256(public).hexdigest()[:32],
                    base64.urlsafe_b64encode(public).rstrip(b"=").decode(),
                    base64.urlsafe_b64encode(bytes(range(16))).rstrip(b"=").decode(), "ABC123"]
        production = DISPLAY_SOURCE.read_text(encoding="utf-8")
        signatures = ["void QrPixelCallback(", "std::unique_ptr<LvglImage> BuildProvisioningQrImage(",
                      "void LcdDisplay::ShowProvisioningQr(", "void LcdDisplay::HideProvisioningQr("]
        code = PREAMBLE + "\n".join(function_source(production, s) for s in signatures)
        code += "\nhutuji::BindIdentitySession session{" + ",".join(map(json.dumps, identity)) + "};\n" + CHECKS
        (temp / "probe.cpp").write_text(code, encoding="utf-8")
        cls.executable = temp / ("probe.exe" if os.name == "nt" else "probe")
        cls.run_command([str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(temp),
                         "-I", str(qr / "include"), "-I", str(ROOT / "main/boards/lichuang-dev"),
                         str(temp / "probe.cpp"), *objects, "-o", str(cls.executable)])

    @classmethod
    def run_command(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                env=cls.env, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr or str(result.returncode))

    def check(self, case):
        self.run_command([str(self.executable), case])

    def test_full_identity_wifi_and_manual_binding(self):
        self.check("identity")

    def test_maximum_escaped_ssid_stays_readable(self):
        self.check("ssid")

    def test_quiet_zone_and_integer_scaled_pixels(self):
        self.check("pixels")

    def test_invalid_payload_and_allocation_failure_recover(self):
        self.check("failure")

    def test_failed_encoding_keeps_notice_and_exit_visible(self):
        self.check("overlay")


PREAMBLE = r'''
#include "qrcode.h"
#include "hutuji_bind_identity_core.h"
#include "hutuji_recovery_core.h"
#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
// 只替换硬件分配与 LVGL 对象存储，不复制产品二维码或显隐算法。
bool fail_allocation = false;
int live_images = 0;
constexpr int MALLOC_CAP_SPIRAM = 1;
constexpr int LV_COLOR_FORMAT_RGB565 = 1;
constexpr unsigned LV_OBJ_FLAG_HIDDEN = 1;
template<class... T> void log_stub(T&&...) {}
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define TAG "QR test"
void* heap_caps_malloc(size_t bytes, int) { return fail_allocation ? nullptr : malloc(bytes); }
struct Descriptor {
    struct { int w, h, stride; } header;
    const uint16_t* data;
};
struct LvglImage { virtual ~LvglImage() = default; virtual const Descriptor* image_dsc() const = 0; };
struct LvglAllocatedImage : LvglImage {
    Descriptor d;
    LvglAllocatedImage(uint16_t* p, size_t, int w, int h, int stride, int) : d{{w,h,stride},p} { ++live_images; }
    ~LvglAllocatedImage() override { free(const_cast<uint16_t*>(d.data)); --live_images; }
    const Descriptor* image_dsc() const override { return &d; }
};
struct lv_obj_t { unsigned flags = 0; const Descriptor* src = nullptr; std::string text; };
void lv_obj_add_flag(lv_obj_t* o,unsigned f) { o->flags |= f; }
void lv_obj_remove_flag(lv_obj_t* o,unsigned f) { o->flags &= ~f; }
bool lv_obj_has_flag(lv_obj_t* o,unsigned f) { return (o->flags & f) != 0; }
void lv_image_set_src(lv_obj_t* o,const Descriptor* p) { o->src = p; }
void lv_label_set_text(lv_obj_t* o,const char* text) { o->text = text; }
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
struct LcdDisplay {
    int height_ = 240;
    lv_obj_t root, code, hint, trigger, voice, wifi, machine, cancel;
    lv_obj_t* provisioning_qr_root_ = &root;
    lv_obj_t* provisioning_qr_code_ = &code;
    lv_obj_t* provisioning_qr_hint_ = &hint;
    lv_obj_t* provisioning_cancel_btn_ = &cancel;
    lv_obj_t* machine_control_trigger_btn_ = &trigger;
    lv_obj_t* voice_talk_btn_ = &voice;
    lv_obj_t* wifi_config_btn_ = &wifi;
    lv_obj_t* machine_control_root_ = &machine;
    lv_obj_t* draw_preview_root_ = nullptr;
    std::unique_ptr<LvglImage> provisioning_qr_image_;
    std::function<void()> provisioning_on_cancel_ = []() {};
    void EnsureProvisioningQrUi() {}
    void ShowProvisioningQr(const std::string&, const std::string&);
    void HideProvisioningQr();
};
'''

CHECKS = r'''
std::string wifi(const std::string& ssid = "Xiaozhi-582C") {
    return hutuji::BuildOpenHotspotWifiQrPayload(ssid,"b8:1f:3f:c4:58:2c") + hutuji::BindIdentityQuery(session);
}
std::string bind() {
    return "https://hutuji.donglicao.com/draw-upload/bind?c=" + session.bind_code + "&m=" +
           hutuji::UrlEncodeQueryComponent("b8:1f:3f:c4:58:2c") + hutuji::BindIdentityQuery(session);
}
void verify_pixels(const std::string& payload, int budget) {
    const auto image = BuildProvisioningQrImage(payload,budget);
    assert(image);
    std::vector<uint8_t> modules;
    esp_qrcode_config_t config{};
    config.display_func_with_cb=QrPixelCallback;
    config.max_qrcode_version=40;
    config.qrcode_ecc_level=ESP_QRCODE_ECC_MED;
    config.user_data=&modules;
    assert(esp_qrcode_generate(&config,payload.c_str())==ESP_OK);
    const int size=static_cast<int>(std::sqrt(modules.size()));
    const auto* d=image->image_dsc();
    const int scale=d->header.w/(size+8);
    assert(scale>=2 && d->header.w==(size+8)*scale && d->header.w<=budget);
    assert(d->header.w==d->header.h && d->header.stride==d->header.w*2);
    for(int y=0;y<d->header.h;++y) for(int x=0;x<d->header.w;++x) {
        const int mx=x/scale-4, my=y/scale-4;
        const bool dark=mx>=0 && my>=0 && mx<size && my<size && modules[my*size+mx];
        assert(d->data[y*d->header.w+x] == (dark?0:65535));
    }
}
int main(int argc,char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    if(mode=="identity") {
        assert(wifi().size()==244 && bind().size()==238);
        for(const auto& payload:{wifi(),bind()}) {
            LcdDisplay ui;
            ui.ShowProvisioningQr(payload,"hint");
            assert(ui.provisioning_qr_image_ && ui.code.src);
            assert(!lv_obj_has_flag(&ui.root,LV_OBJ_FLAG_HIDDEN));
        }
    } else if(mode=="ssid") {
        const auto payload=wifi(std::string(32,';'));
        assert(payload.size()==328);
        LcdDisplay ui;
        ui.ShowProvisioningQr(payload,"hint");
        assert(ui.provisioning_qr_image_ && ui.code.src);
        assert(ui.code.src->header.w>=154);
    } else if(mode=="pixels") {
        for(const auto& payload:{wifi(),bind(),wifi(std::string(32,';'))})
            for(int budget:{160,220}) verify_pixels(payload,budget);
    } else if(mode=="failure") {
        assert(!BuildProvisioningQrImage("",160));
        assert(!BuildProvisioningQrImage(std::string(600,'a'),160));
        assert(!BuildProvisioningQrImage(wifi(),80));
        fail_allocation=true;
        assert(!BuildProvisioningQrImage(wifi(),160));
        fail_allocation=false;
        assert(BuildProvisioningQrImage(wifi(),160));
    } else if(mode=="overlay") {
        LcdDisplay ui;
        for(int i=0;i<3;++i) {
            ui.ShowProvisioningQr(wifi(),"original hint");
            assert(ui.code.src && live_images==1);
            fail_allocation=true;
            ui.ShowProvisioningQr(wifi(),"original hint");
            fail_allocation=false;
            assert(live_images==0 && !ui.code.src);
            assert(!lv_obj_has_flag(&ui.root,LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(&ui.cancel,LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(&ui.code,LV_OBJ_FLAG_HIDDEN));
            assert(!ui.hint.text.empty() && ui.hint.text!="original hint");
            assert(lv_obj_has_flag(&ui.trigger,LV_OBJ_FLAG_HIDDEN));
            ui.ShowProvisioningQr(wifi(),"retry hint");
            assert(ui.code.src && !lv_obj_has_flag(&ui.code,LV_OBJ_FLAG_HIDDEN));
            assert(ui.hint.text=="retry hint");
            ui.HideProvisioningQr();
            assert(live_images==0 && !ui.code.src);
            assert(lv_obj_has_flag(&ui.root,LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(&ui.trigger,LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(&ui.voice,LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(&ui.wifi,LV_OBJ_FLAG_HIDDEN));
        }
    } else { assert(false); }
    assert(live_images==0);
}
'''


if __name__ == "__main__":
    unittest.main()
