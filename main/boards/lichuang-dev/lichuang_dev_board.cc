#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp32_camera.h"
#include "hutuji_ble_diag.h"
#include "hutuji_conversation_report.h"
#include "hutuji_job.h"
#include "hutuji_music.h"
#include "hutuji_pipe.h"
#include "i2c_device.h"
#include "mcp_server.h"
#include "plotter_provision.h"
#include "press_to_talk_mcp_tool.h"
#include "wifi_board.h"

// 调试用：需要本地工程回归时，在编译参数里同时定义：
//   HUTUJI_AUTO_TEST_DRAW
//   HUTUJI_AUTO_TEST_DRAW_URL="http://<host>/files/<token>.gcode"
//   HUTUJI_AUTO_TEST_DRAW_PREVIEW_URL="http://<host>/files/<token>.png"
// 两个 URL 都取自同一次云端 hutuji_draw 返回，脚手架走的是与产品一致的
// 预览→确认两段路径（不绕过确认门），否则 §8.5 取证不能代表产品行为。
// 默认关闭；禁止把一次性 URL 或真实凭据写进源码。
// #define HUTUJI_AUTO_TEST_DRAW
#ifndef HUTUJI_AUTO_TEST_DRAW_URL
#define HUTUJI_AUTO_TEST_DRAW_URL ""
#endif
#ifndef HUTUJI_AUTO_TEST_DRAW_PREVIEW_URL
#define HUTUJI_AUTO_TEST_DRAW_PREVIEW_URL ""
#endif

// bringup §8.5（换纸中调 abort）取证镜像专用：
//   HUTUJI_AUTO_TEST_ABORT_ON_PAPER
// 换纸保护窗只有约 10s，云端/人手下发都难稳定命中，故由本地在窗口开启的第一时间
// 调 hutuji.abort 等价入口。需与 HUTUJI_AUTO_TEST_DRAW 搭配才会自动产生窗口。
// 默认关闭；取证完成后必须回刷不带此宏的镜像，别让脚手架进产品镜像。

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <cJSON.h>
#include <lvgl.h>
#include <stdexcept>

#include <wifi_manager.h>
#include "display/lvgl_display/lvgl_theme.h"
#include "hutuji_recovery_core.h"

#define TAG "LichuangDevBoard"

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557)
        : BoxAudioCodec(i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC,
                        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE,
                        28.0f,  // Physical MIC1 gain
                        2,      // Physical MIC3 is the playback reference input
                        0.0f),
          pca9557_(pca9557) {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Display* display_;
    Pca9557* pca9557_;
    Esp32Camera* camera_;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode
            // without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // In press-to-talk mode the click event is handled by press down/up
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });

        boot_button_.OnPressDown([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,
            .y_max = DISPLAY_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC,  // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = 1,
                    .mirror_x = 1,
                    .mirror_y = 0,
                },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags = {
                .disable_control_phase = 1,
            }};
        tp_io_config.scl_speed_hz = 400000;

        esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        assert(tp);

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };

        if (touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
        }
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
                           "End this conversation and enter WiFi configuration mode.\n"
                           "**CAUTION** You must ask the user to confirm this action.",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });

        // Allow switching between press-to-talk (长按说话) and click-to-talk (单击唤醒)
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();

        // 对话上报 worker 须尽早用静态栈创建，勿拖到首句 STT（heap 低谷会失败）。
        hutuji::InitConversationReport();

        // hutuji 写字机 Telnet 哑管道（方案 E：TCP 客户端 → Grbl_Esp32 Telnet:23）
        hutuji::Pipe::GetInstance().Start();

        // BLE-DIAG 阶段 A 只读诊断广播；默认关闭，未启用时是空实现。
        hutuji::ble_diag::Start();

        mcp_server.AddTool(
            "hutuji.status",
            "查询本机与写字机的 Telnet 管道：是否已连接、Grbl "
            "是否就绪、是否授权、任务状态、最近应答行。"
            "用户问「写字机连上了吗/能不能动」时用；云端生成服务请用 hutuji_status。"
            "state 字段含义：idle 空闲、previewing 预览加载中、awaiting_confirmation 等用户确认、"
            "downloading 下载中、verifying 校验中、streaming 绘图中、"
            "paused 已暂停、paper_change 换纸中、reconnecting 断线恢复中、done 完成、"
            "error 出错、aborted 已取消。",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().StatusJson();
            });

        mcp_server.AddTool(
            "hutuji.draw",
            "先出预览：参数 url 是云端 hutuji_draw 返回的 G-code 地址，preview_url 是同一次返回的 "
            "PNG 预览地址。本工具只把预览显示到设备屏幕上，不会启动任何机械动作。"
            "屏幕会出现「开始画」「取消」两个按钮，用户也可以直接说开始画或取消；"
            "用户确认后才调 hutuji.confirm 真正出图。"
            "用户只说「画一只猫」时应先走云端 hutuji_draw，不要瞎编 url。",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("preview_url", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string& url = properties["url"].value<std::string>();
                const std::string& preview_url = properties["preview_url"].value<std::string>();
                return hutuji::Job::GetInstance().StartDraw(url, preview_url);
            });

        mcp_server.AddTool("hutuji.confirm",
                           "用户看过屏幕预览后确认出图：说「开始画/可以/就这个/好看」时用。"
                           "只在 hutuji.status 的 state 为 awaiting_confirmation 时有效；"
                           "没有待确认预览时会返回错误，不要凭空调用。"
                           "用户点屏幕上的「开始画」按钮与本工具等效。",
                           PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                               return hutuji::Job::GetInstance().RequestConfirm();
                           });

        mcp_server.AddTool("hutuji.abort",
                           "中止当前绘图转发，或取消尚未确认的预览。用户说停下/取消/不要这个时用。"
                           "取消预览不会碰写字机；已在绘图时若正在换纸，可能无法立刻停，"
                           "完成后才会停——须如实告诉用户。",
                           PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                               return hutuji::Job::GetInstance().RequestAbort();
                           });

        mcp_server.AddTool(
            "hutuji.pause",
            "暂停当前绘图（笔停在原地，进度不丢）。用户说「暂停/等一下/停一下」时用。"
            "与 hutuji.abort 不同：pause 可以用 hutuji.resume 接着画，abort 是彻底放弃。"
            "换纸中无法暂停，此时会如实返回提示。",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestPause();
            });

        mcp_server.AddTool("hutuji.resume",
                           "恢复之前暂停的绘图，从暂停处接着画。用户说「继续/接着画」时用。",
                           PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                               return hutuji::Job::GetInstance().RequestResume();
                           });

        mcp_server.AddTool(
            "hutuji.repeat",
            "把上一张画再画一遍（复用设备里存的 G-code，不用重新生成，也不用再问云端）。"
            "用户说「再画一张/再来一个/一样的再画一次」时用。"
            "注意：需要先换上白纸；没画过东西时会返回错误。",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestRepeat();
            });

        mcp_server.AddTool(
            "hutuji.pen_test",
            "笔测试：落笔停 1 秒再抬笔，用来确认笔能不能碰到纸、有没有墨。"
            "用户说「试一下笔/笔能用吗/怎么画不出来」时用。测完让用户看纸上有没有点。",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestPenTest();
            });

        mcp_server.AddTool(
            "hutuji.manual",
            "手动控制写字机轴运动，仅空闲可用。action 取值："
            "\"jog_x+\"/\"jog_x-\"/\"jog_y+\"/\"jog_y-\" 按当前步距点动（步距用 "
            "\"jog_step_1\"/\"jog_step_10\" 切 1mm/10mm；方向：左=X- 右=X+ 前=Y+ 后=Y-）；"
            "\"pen_up\" 抬笔、\"pen_down\" 落笔（落笔先做 Z0 校准，笔尖会碰纸；"
            "已落笔时重复调用无动作，返回 已处于落笔状态）；\"home\" 回左下原点。"
            "用户说「往左/右/前/后挪一点」「抬笔/落笔」「回原点」「步距调大/调小」时用。"
            "返回 started 表示已开始执行，完成后设备会播报；正忙或未连接返回 error。"
            "set_origin/unlock/motor_off/reset 是维护动作，语音不开放。",
            PropertyList({Property("action", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string action = properties["action"].value<std::string>();
                if (!hutuji::IsVoiceAllowedAction(action)) {
                    return std::string("{\"error\":\"语音不开放该手动动作\"}");
                }
                return hutuji::Job::GetInstance().RequestManualControl(action);
            });

        mcp_server.AddTool(
            "hutuji.sing",
            "播放歌曲：url 是云端 hutuji_sing 返回的歌曲地址，title 是歌名。"
            "只放歌不碰写字机；下载完成后自动开始唱，唱完自动停。"
            "想换一首直接再调本工具（自动切歌）；用户说停下时用 hutuji.stop_song。"
            "不要瞎编 url——用户点歌时应先走云端 hutuji_sing 查目录。",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("title", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string& url = properties["url"].value<std::string>();
                const std::string& title = properties["title"].value<std::string>();
                auto& music = hutuji::HutujiMusic::GetInstance();
                if (music.Play(url, title)) {
                    return std::string("{\"ok\":true}");
                }
                return std::string("{\"ok\":false,\"error\":\"") + music.LastError() +
                       "\"}";
            });

        mcp_server.AddTool("hutuji.stop_song",
                           "停止当前播放的歌曲。用户说别唱了/停下/安静时用。没歌在放时调用也安全。",
                           PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                               hutuji::HutujiMusic::GetInstance().Stop();
                               return true;
                           });

#ifdef HUTUJI_AUTO_TEST_DRAW
        xTaskCreate(
            [](void*) {
                auto& pipe = hutuji::Pipe::GetInstance();
                for (int i = 0; i < 60; ++i) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (pipe.IsConnected() && pipe.IsReady())
                        break;
                }
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (pipe.IsConnected() && pipe.IsReady()) {
                    const char* url = HUTUJI_AUTO_TEST_DRAW_URL;
                    const char* preview_url = HUTUJI_AUTO_TEST_DRAW_PREVIEW_URL;
                    if (url[0] == '\0' || preview_url[0] == '\0') {
                        ESP_LOGE("AutoTest",
                                 "已启用 HUTUJI_AUTO_TEST_DRAW，但未同时定义 "
                                 "HUTUJI_AUTO_TEST_DRAW_URL 与 HUTUJI_AUTO_TEST_DRAW_PREVIEW_URL");
                        vTaskDelete(nullptr);
                        return;
                    }
                    auto& job = hutuji::Job::GetInstance();
                    ESP_LOGW("AutoTest", "触发自动测试绘图（预览段）");
                    auto result = job.StartDraw(url, preview_url);
                    ESP_LOGW("AutoTest", "StartDraw 返回: %s", result.c_str());
                    // 脚手架不绕过确认门：等预览下载到 awaiting_confirmation 后再自动确认，
                    // 与用户点屏幕「开始画」走同一入口。预览失败则 state 不会是待确认，
                    // 此处超时退出，不强行开跑。
                    bool confirmed = false;
                    for (int i = 0; i < 120; ++i) {
                        vTaskDelay(pdMS_TO_TICKS(500));
                        if (job.StatusJson().find("\"awaiting_confirmation\"") == std::string::npos) {
                            continue;
                        }
                        auto confirm_result = job.RequestConfirm();
                        ESP_LOGW("AutoTest", "RequestConfirm 返回: %s", confirm_result.c_str());
                        confirmed = true;
                        break;
                    }
                    if (!confirmed) {
                        ESP_LOGE("AutoTest", "预览未进入 awaiting_confirmation，放弃自动确认；status: %s",
                                 job.StatusJson().c_str());
                    }
                }
                vTaskDelete(nullptr);
            },
            "auto_test", 4096, nullptr, 3, nullptr);
#endif

#ifdef HUTUJI_AUTO_TEST_ABORT_ON_PAPER
        // bringup §8.5（换纸中调 abort）专用脚手架。设备侧 abort 唯一入口是云端
        // MCP 工具 hutuji.abort，而换纸保护窗只有约 10s（B-13 实测 0.14s→10.11s），
        // 语音/控制台下发都难稳定命中；这里在 paper_active 翻真的第一时间本地调用。
        // 默认不定义此宏，正常镜像行为不变；取证后必须回刷不带宏的镜像。
        // 注意：paper_active 也覆盖断连恢复保护窗，故触发前后都打 status 自证窗口类型。
        xTaskCreate(
            [](void*) {
                auto& job = hutuji::Job::GetInstance();
                // ponytail: 50ms 轮询、20 分钟上限，够覆盖单张 A4 出图到页尾换纸；
                // 超时即退出，不让脚手架任务常驻。上限不够就调大循环次数。
                for (int i = 0; i < 24000; ++i) {
                    if (job.IsPaperActive()) {
                        ESP_LOGW("AutoTest", "换纸窗口命中，abort 前 status: %s",
                                 job.StatusJson().c_str());
                        auto result = job.RequestAbort();
                        ESP_LOGW("AutoTest", "换纸中 abort 返回: %s", result.c_str());
                        ESP_LOGW("AutoTest", "abort 后 status: %s", job.StatusJson().c_str());
                        vTaskDelete(nullptr);
                        return;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                ESP_LOGW("AutoTest", "20 分钟内未观察到换纸窗口，abort 脚手架退出");
                vTaskDelete(nullptr);
            },
            "auto_test_abort", 3072, nullptr, 4, nullptr);
#endif
    }

public:
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                if (event == NetworkEvent::WifiConfigModeEnter) {
                    const std::string ap_ssid = WifiManager::GetInstance().GetApSsid();
                    display_->ShowProvisioningQr(
                        hutuji::BuildOpenHotspotWifiQrPayload(ap_ssid),
                        "Scan: " + ap_ssid + "\nOpen: " + WifiManager::GetInstance().GetApWebUrl());
                } else if (event == NetworkEvent::WifiConfigModeExit ||
                           event == NetworkEvent::Connected) {
                    display_->HideProvisioningQr();
                }
                if (event == NetworkEvent::Connected) {
                    // 户网连上后巡检写字机：找不到且出厂热点在场则自动跳配
                    // （零接触配网；用户只扫过一次码）。
                    hutuji::PlotterProvision::GetInstance().OnHomeNetworkConnected();
                }
                if (callback) {
                    callback(event, data);
                }
            });
    }

    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeButtons();
        InitializeCamera();
        InitializeTools();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(i2c_bus_, pca9557_);
        return &audio_codec;
    }
    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override { return camera_; }

    // 与 waveshare 板同款（esp32-s3-touch-lcd-3.5.cc SetPowerSaveLevel）：
    // ①出图活跃窗口内拒绝任何省电回落——音频通道关闭（application.cc
    //   OnAudioChannelClosed）每几秒无条件踩回，与 Job 的 PERFORMANCE 持有互踩
    //   造成 Telnet RTT 尖峰（实测 ok 间隔 200-290ms，笔运动肉眼卡顿）。
    // ②稳态 LOW_POWER(MAX_MODEM) 降为 BALANCED(MIN_MODEM)：MAX_MODEM 长睡眠是
    //   写字机断联（WiFi reason 3 AP 去关联 + errno=113，8 份日志）与慢发现
    //   （首包 ~200ms、RTT p50 117-134ms）的头号嫌疑（2026-08-23 取证）。
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::LOW_POWER) {
            level = PowerSaveLevel::BALANCED;
        }
        if (level != PowerSaveLevel::PERFORMANCE &&
            hutuji::Job::GetInstance().HoldsPerformanceForRadio()) {
            return;
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(LichuangDevBoard);
