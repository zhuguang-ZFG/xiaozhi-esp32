#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "system_info.h"
#include "boards/lichuang-dev/hutuji_job.h"
#include "boards/lichuang-dev/hutuji_pipe.h"
#include "boards/lichuang-dev/hutuji_draw_bind.h"
#include "boards/lichuang-dev/hutuji_ble_diag.h"
#include "boards/lichuang-dev/hutuji_ota.h"
#include "boards/lichuang-dev/hutuji_memory.h"
#include "boards/lichuang-dev/hutuji_conversation_report.h"
#include "boards/lichuang-dev/hutuji_recovery_core.h"
#include "boards/lichuang-dev/hutuji_music.h"
#include "boards/lichuang-dev/plotter_provision.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "adc_battery_monitor.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <cstring>

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <wifi_manager.h>

#include "led/single_led.h"
#include "system_reset.h"
#include "esp_lcd_ili9341.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>

#define TAG "FreenoveESP32S3Display"

// 2026-09-08 小派换板落地（lichuang-dev → LCDWiki ES3C28P 2.8"）：硬件层沿用上游
// freenove 板（README 自述与 ES3C28P/ES3N28P 同设计，引脚已逐项与 lcdwiki
// 规格页对账：I2S 4/5/6/7/8、PA=1、编解码与触摸共 I2C 15/16、屏 SPI 10/11/12/13/46、
// 背光 45、BOOT=0、RGB=42、电池 ADC=9）。本文件在上游骨架上接入 hutuji 功能集：
// 触摸换 LVGL indev（预览确认/取消按钮必需，上游只做点按手势）、hutuji MCP 工具
// 与配网/绑定/看门狗钩子镜像 waveshare esp32-s3-touch-lcd-3.5.cc（两板同为
// SpiLcdDisplay + FT5x06 系触摸 + ES8311，行为锚定一台写字机控制台）。
class FreenoveESP32S3Display : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay *display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    AdcBatteryMonitor* adc_battery_monitor_;
    // 断连看门狗（与 waveshare 同口径）：已配网但持续连不上时自动显配网二维码，
    // 免拆机免串口救回。配网跳窗（PlotterProvision.IsBusy）期间不武装。
    esp_timer_handle_t wifi_lost_timer_ = nullptr;

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    void InitializeTouch() {
        // FT6336G @0x38（FT5x06 系默认地址）挂编解码同一条 I2C 总线（SDA16/SCL15）。
        // RST=IO18/INT=IO17 规格页在册但暂不接线：与 lichuang/waveshare 同走 NC 轮询，
        // HIL 若见触摸异常再启用硬复位。坐标映射先套 lichuang 同款（swap+mirror_x），
        // 240x320 面板的镜像方向以首刷 HIL 点按实测校准。
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,
            .y_max = DISPLAY_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC,
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

        esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle);
        esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        assert(tp);

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lv_indev_t* touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (touch_indev != nullptr) {
            // waveshare HIL 坐实的两条通用 FT6336×LVGL 加固（与具体面板无关）：
            // 静止按压抖动超 LVGL 默认 10px 滚动阈值会把点按误判成拖动吃掉 CLICKED，
            // 提到 24px；indev 读定时器默认 33ms 是「不跟手」主延迟源，提到 10ms。
            lv_indev_set_scroll_limit(touch_indev, 24);
            lv_timer_set_period(lv_indev_get_read_timer(touch_indev), 10);
        }
        ESP_LOGI(TAG, "Touch panel initialized (FT6336G, LVGL indev)");
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MIS0_PIN;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "Install LCD driver ILI9341");
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 初始化工具（镜像 waveshare esp32-s3-touch-lcd-3.5.cc，工具描述逐字一致防双板漂移）
    using MachineControlRequest = std::string (hutuji::Job::*)();

    static std::string MachineControlFeedback(const std::string& result) {
        cJSON* root = cJSON_Parse(result.c_str());
        if (root == nullptr) {
            return Lang::Strings::MACHINE_ACTION_FAILED;
        }
        std::string message = Lang::Strings::MACHINE_ACTION_SENT;
        if (cJSON_IsString(root)) {
            const char* value = cJSON_GetStringValue(root);
            if (value != nullptr) {
                if (strcmp(value, "ok") == 0) {
                    message = Lang::Strings::MACHINE_ACTION_SENT;
                } else if (strcmp(value, "started") == 0 ||
                           strcmp(value, "started_redownload") == 0) {
                    message = Lang::Strings::MACHINE_ACTION_STARTED;
                } else {
                    message = value;
                }
            }
        } else if (cJSON_IsObject(root)) {
            const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (cJSON_IsString(error) && error->valuestring != nullptr) {
                message = error->valuestring;
            }
        }
        cJSON_Delete(root);
        return message;
    }

    void ScheduleMachineControl(const char* action, MachineControlRequest request) {
        ESP_LOGI(TAG, "ui machine action=%s", action);
        Application::GetInstance().Schedule([this, request]() {
            const std::string result = (hutuji::Job::GetInstance().*request)();
            display_->ShowNotification(MachineControlFeedback(result));
        });
    }
    void ScheduleManualControl(const char* action) {
        ESP_LOGI(TAG, "ui machine action=%s", action);
        const std::string act = action;
        Application::GetInstance().Schedule([this, act]() {
            const std::string result = hutuji::Job::GetInstance().RequestManualControl(act);
            display_->ShowNotification(MachineControlFeedback(result));
        });
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });

        // 对话上报 worker 须尽早用静态栈创建，勿拖到首句 STT（heap 低谷会失败）。
        hutuji::InitConversationReport();

        // hutuji 写字机 Telnet 哑管道（TCP 客户端 → Grbl_Esp32 Telnet:23）。
        hutuji::Pipe::GetInstance().Start();

        // BLE-DIAG 阶段 A 只读诊断广播；默认关闭，未启用时是空实现。
        hutuji::ble_diag::Start();

        hutuji::ota::RegisterTools(mcp_server);
        hutuji::memory::RegisterTools(mcp_server);

        display_->ConfigureMachineControls(
            [this]() { ScheduleMachineControl("pause", &hutuji::Job::RequestPause); },
            [this]() { ScheduleMachineControl("resume", &hutuji::Job::RequestResume); },
            [this]() { ScheduleMachineControl("abort", &hutuji::Job::RequestAbort); },
            [this]() { ScheduleMachineControl("repeat", &hutuji::Job::RequestRepeat); },
            [this]() { ScheduleMachineControl("pen_test", &hutuji::Job::RequestPenTest); },
            [this](const char* action) { ScheduleManualControl(action); },
            []() { hutuji::PlotterProvision::GetInstance().RequestManual(); });

        // boot 键功能上屏：「说话」与 boot 单击完全同语义（starting 态转配网，
        // 否则 ToggleChatState）。「配网」直接进配网模式，屏显二维码。
        display_->ConfigureVoiceEntry(
            [this]() {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            },
            [this]() { EnterWifiConfigMode(); });
        // 二维码「关闭」= 退出配网：StopConfigAp→ConfigModeExit→WifiBoard 自动
        // TryWifiConnect。必须 Schedule 回主循环：StopConfigAp 的事件回调是同步调用，
        // 新机无凭据时 TryWifiConnect 内部有 vTaskDelay(1500)，在 taskLVGL 上跑会卡死 UI。
        display_->SetProvisioningCancelHandler([this]() {
            if (hutuji::IsDrawBindActive()) {
                hutuji::StopDrawBind(display_);
                return;
            }
            Application::GetInstance().Schedule(
                []() { WifiManager::GetInstance().StopConfigAp(); });
        });

        display_->ConfigureDrawBind([this]() {
            Application::GetInstance().Schedule([this]() {
                hutuji::StartDrawBind(display_);
            });
        });

        mcp_server.AddTool("hutuji.status",
            "查询本机与写字机的 Telnet 管道：是否已连接、Grbl 是否就绪、任务状态。"
            "state 含 previewing 预览加载中、awaiting_confirmation 等用户确认。",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().StatusJson();
            });

        mcp_server.AddTool("hutuji.draw",
            "先出预览：url 是云端 hutuji_draw 返回的 G-code 地址，preview_url 是同一次返回的 PNG "
            "预览地址。只把预览显示到屏幕上，不启动任何机械动作；屏幕会出现「开始画」「取消」"
            "按钮，用户确认后才调 hutuji.confirm。"
            "写文章/写诗多页时：把云端 hutuji_write 返回的 pages 数组原样转成 JSON 字符串传给 "
            "pages 参数（形如 [{\"url\":\"...\",\"preview_url\":\"...\"}, ...]），并把第 1 页两个地址"
            "同时填到 url/preview_url；设备逐页写、每页写完自动换纸。单页出图时 pages 留空。",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("preview_url", kPropertyTypeString),
                          Property("pages", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string& url = properties["url"].value<std::string>();
                const std::string& preview_url = properties["preview_url"].value<std::string>();
                const std::string& pages = properties["pages"].value<std::string>();
                return hutuji::Job::GetInstance().StartDraw(url, preview_url, pages);
            });

        mcp_server.AddTool("hutuji.confirm",
            "用户看过屏幕预览后确认出图：说「开始画/可以/就这个」时用。"
            "仅在 state 为 awaiting_confirmation 时有效；等价于用户点屏幕「开始画」按钮。",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestConfirm();
            });

        mcp_server.AddTool("hutuji.abort", "中止当前绘图转发，或取消尚未确认的预览。",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestAbort();
            });

        mcp_server.AddTool("hutuji.pause", "暂停当前绘图（可恢复）。", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestPause();
            });

        mcp_server.AddTool("hutuji.resume", "恢复之前暂停的绘图。", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestResume();
            });

        mcp_server.AddTool("hutuji.repeat", "把上一张画再画一遍。", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestRepeat();
            });

        mcp_server.AddTool("hutuji.pen_test", "笔测试：落笔停 1 秒再抬笔。", PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                return hutuji::Job::GetInstance().RequestPenTest();
            });

        // 语音手动控制：描述与 lichuang_dev_board 保持逐字一致，避免双板行为漂移。
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

        // 唱歌与绘图完全解耦：只走 AudioService 播放泵，不碰写字机管道。
        // 工具描述与 lichuang_dev_board 保持逐字一致，避免双板行为漂移。
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
    }

    // 同室部署写字机：10dBm(40=0.25dBm 单位) 足够覆盖，显著降低 Wi-Fi 峰值电流。
    void StartNetwork() override {
        WifiBoard::StartNetwork();
        const esp_err_t tx_err = esp_wifi_set_max_tx_power(40);
        if (tx_err != ESP_OK) {
            ESP_LOGW(TAG, "set max tx power failed: %s", esp_err_to_name(tx_err));
        }
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                if (event == NetworkEvent::WifiConfigModeEnter) {
                    // BuildOpenHotspotWifiQrPayload(ap_ssid, SystemInfo::GetMacAddress())
                    // 配网二维码在 NetworkEvent::Connected 之前显示；此顺序是回连体验契约。
                    StopWifiLostWatchdog();
                    const std::string ap_ssid = WifiManager::GetInstance().GetApSsid();
                    const std::string qr =
                        hutuji::BuildIdentityWifiQrPayload(ap_ssid, SystemInfo::GetMacAddress());
                    if (qr.empty()) {
                        display_->HideProvisioningQr();
                        display_->ShowNotification("配网身份暂不可用，请重启后重试", 5000);
                    } else {
                        display_->ShowProvisioningQr(
                            qr, "Scan: " + ap_ssid +
                                    "\nOpen: " + WifiManager::GetInstance().GetApWebUrl());
                    }
                } else if (event == NetworkEvent::WifiConfigModeExit ||
                           event == NetworkEvent::Connected) {
                    StopWifiLostWatchdog();
                    display_->HideProvisioningQr();
                } else if (event == NetworkEvent::Disconnected) {
                    // 配网跳窗内的断连是流程一部分：看门狗若在 120s 触发会把
                    // 回切途中的设备踹进配网模式，跳窗期间不武装。
                    if (!hutuji::PlotterProvision::GetInstance().IsBusy()) {
                        StartWifiLostWatchdog();
                    }
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

    void StartWifiLostWatchdog() {
        constexpr uint64_t kWifiLostTimeoutUs = 120ULL * 1000 * 1000;
        if (wifi_lost_timer_ == nullptr) {
            const esp_timer_create_args_t args = {
                .callback = [](void* arg) {
                    auto* self = static_cast<FreenoveESP32S3Display*>(arg);
                    // 先原地续表再 Schedule：EnterWifiConfigMode 对 Connecting/配网中/
                    // 升级中等状态门控早退，one-shot 不重武装会让安全网静默失效。
                    self->StartWifiLostWatchdog();
                    // esp_timer 任务上下文不直接碰网络状态机：Schedule 回主循环执行。
                    Application::GetInstance().Schedule([self]() {
                        if (WifiManager::GetInstance().IsConnected()) {
                            return;
                        }
                        ESP_LOGW(TAG, "WiFi lost >120s, entering config mode (QR on screen)");
                        self->EnterWifiConfigMode();
                    });
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "wifi_lost_wd",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &wifi_lost_timer_) != ESP_OK) {
                wifi_lost_timer_ = nullptr;
                ESP_LOGW(TAG, "wifi lost watchdog create failed");
                return;
            }
        }
        // 重复 Disconnected 重新起表：看的是「最后一次断连起持续 120s 未恢复」。
        esp_timer_stop(wifi_lost_timer_);
        esp_timer_start_once(wifi_lost_timer_, kWifiLostTimeoutUs);
    }

    void StopWifiLostWatchdog() {
        if (wifi_lost_timer_ != nullptr) {
            esp_timer_stop(wifi_lost_timer_);
        }
    }

public:
    FreenoveESP32S3Display(): boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, AUDIO_CODEC_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override { return display_; }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }

    // 与 lichuang/waveshare 同口径：LOW_POWER(MAX_MODEM) 降 BALANCED(MIN_MODEM)，
    // 出图活跃窗口内拒绝一切非 PERFORMANCE 回落（防 Telnet RTT 尖峰笔运动卡顿）。
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

DECLARE_BOARD(FreenoveESP32S3Display);
