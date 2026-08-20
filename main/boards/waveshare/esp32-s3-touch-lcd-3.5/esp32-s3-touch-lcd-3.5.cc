#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "boards/lichuang-dev/hutuji_job.h"
#include "boards/lichuang-dev/hutuji_pipe.h"
#include "boards/lichuang-dev/hutuji_ble_diag.h"
#include "boards/lichuang-dev/hutuji_recovery_core.h"
#include "boards/lichuang-dev/hutuji_music.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <cstring>

#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <esp_timer.h>
#include <esp_wifi.h>
#include <wifi_manager.h>
#include "esp_io_expander_tca9554.h"

#include "axp2101.h"
#include "power_save_timer.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>

#include "esp_video.h"

#define TAG "waveshare_lcd_3_5"

class Pmic : public Axp2101 {
    uint8_t boot_poweroff_status_ = 0;
    uint8_t boot_status1_ = 0;
    uint8_t boot_status2_ = 0;
    uint8_t boot_input_current_limit_ = 0;
    uint8_t boot_irq_status1_ = 0;
    uint8_t boot_irq_status2_ = 0;
    uint8_t boot_irq_status3_ = 0;

    public:
        Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
            // 0x21 和 IRQ 状态会跨 PMIC 关机锁存；必须在任何电源配置写入前取证。
            boot_poweroff_status_ = ReadReg(0x21);
            boot_status1_ = ReadReg(0x00);
            boot_status2_ = ReadReg(0x01);
            boot_input_current_limit_ = ReadReg(0x16);
            boot_irq_status1_ = ReadReg(0x48);
            boot_irq_status2_ = ReadReg(0x49);
            boot_irq_status3_ = ReadReg(0x4A);
            LogBootStatus();
            // AXP2101 0x10 bit2 对应 disablePwronShutPMIC()；0x22 bit1
            // 对应 disableLongPressShutdown()。0x22 bit2 是过温关机保护，必须保留。
            const uint8_t common_config = ReadReg(0x10);
            WriteReg(0x10, common_config & ~0x04);
            const uint8_t poweroff_enable = ReadReg(0x22);
            WriteReg(0x22, (poweroff_enable | 0x04) & ~0x02);
            // DCDC1 供 ESP32-S3、LCD、触摸和相机。bit2=Always PWM 改善语音播报与
            // Wi-Fi/Telnet 并发时的负载阶跃响应；bits1:0=11 把 UVP 防抖从默认
            // 60us 提到 240us（数据手册 0x81），吸收 NS4150B 功放阶跃造成的瞬态
            // 下陷；UVP 阈值与过温保护全部保留，持续欠压仍会断电。
            const uint8_t dc_force_pwm = ReadReg(0x81);
            WriteReg(0x81, dc_force_pwm | 0x07);
            const uint8_t configured_dc_mode = ReadReg(0x81);
            ESP_LOGW(TAG, "pmic dcdc1 mode=%s reg81=0x%02x",
                     (configured_dc_mode & 0x04) != 0 ? "always-pwm" : "auto",
                     configured_dc_mode);
    
            // Disable All DCs but DC1
            WriteReg(0x80, 0x01);
            // Disable All LDOs
            WriteReg(0x90, 0x00);
            WriteReg(0x91, 0x00);
    
            // Set DC1 to 3.3V
            WriteReg(0x82, (3300 - 1500) / 100);
    
            // Set ALDO1 to 3.3V
            WriteReg(0x92, (3300 - 500) / 100);

            WriteReg(0x96, (1500 - 500) / 100);
            WriteReg(0x97, (2800 - 500) / 100);
    
            // Enable ALDO1 BLDO1 BLDO2 
            WriteReg(0x90, 0x31);
        
            WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V
            
            WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
            WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
            WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
        }

        void LogBootStatus() const {
            ESP_LOGW(TAG,
                     "pmic boot status off=0x%02x status=0x%02x/0x%02x ilim=0x%02x irq=0x%02x/0x%02x/0x%02x",
                     boot_poweroff_status_, boot_status1_, boot_status2_, boot_input_current_limit_,
                     boot_irq_status1_, boot_irq_status2_, boot_irq_status3_);
        }
    };

typedef struct {
    int cmd;                /*<! The specific LCD command */
    const void *data;       /*<! Buffer that holds the command specific data */
    size_t data_bytes;      /*<! Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*<! Delay in milliseconds after this command */
} st7796_lcd_init_cmd_t;

typedef struct {
    const st7796_lcd_init_cmd_t *init_cmds;     /*!< Pointer to initialization commands array. Set to NULL if using default commands.
                                                 *   The array should be declared as `static const` and positioned outside the function.
                                                 *   Please refer to `vendor_specific_init_default` in source file.
                                                 */
    uint16_t init_cmds_size;                    /*<! Number of commands in above array */
} st7796_vendor_config_t;

st7796_lcd_init_cmd_t st7796_lcd_init_cmds[] = {
    {0x11, (uint8_t []){ 0x00 }, 0, 120},

    // {0x36, (uint8_t []){ 0x08 }, 1, 0},

    {0x3A, (uint8_t []){ 0x05 }, 1, 0},
    {0xF0, (uint8_t []){ 0xC3 }, 1, 0},
    {0xF0, (uint8_t []){ 0x96 }, 1, 0},
    {0xB4, (uint8_t []){ 0x01 }, 1, 0},
    {0xB7, (uint8_t []){ 0xC6 }, 1, 0},
    {0xC0, (uint8_t []){ 0x80, 0x45 }, 2, 0},
    {0xC1, (uint8_t []){ 0x13 }, 1, 0},
    {0xC2, (uint8_t []){ 0xA7 }, 1, 0},
    {0xC5, (uint8_t []){ 0x0A }, 1, 0},
    {0xE8, (uint8_t []){ 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, (uint8_t []){ 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14, 0},
    {0xE1, (uint8_t []){ 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32},14, 0},
    {0xF0, (uint8_t []){ 0x3C }, 1, 0},
    {0xF0, (uint8_t []){ 0x69 }, 1, 120},
    {0x21, (uint8_t []){ 0x00 }, 0, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},
};

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Pmic* pmic_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    EspVideo* camera_;
    // 断连看门狗（2026-08-20「找不到 WiFi 直接显二维码」决策）：已配网但持续
    // 连不上（换路由器/改密码/AP 关机）时，上游只在 WifiManager 内无限重连、
    // 永不进配网模式，用户只能摸实体 boot 键。Disconnected 起 120s 定时，
    // Connected/进配网即撤；到期自动进配网、屏显二维码，全程无按键。
    esp_timer_handle_t wifi_lost_timer_ = nullptr;

    void ReplayPmicBootStatusAfterUsbReady() {
        Pmic* pmic = pmic_;
        BaseType_t created = xTaskCreate(
            [](void* arg) {
                auto* pmic = static_cast<Pmic*>(arg);
                vTaskDelay(pdMS_TO_TICKS(15000));
                pmic->LogBootStatus();
                vTaskDelete(nullptr);
            },
            "pmic_boot_log", 2048, pmic, 1, nullptr);
        if (created != pdTRUE) {
            ESP_LOGE(TAG, "failed to create delayed PMIC boot log task");
        }
    }

    void InitializePowerSaveTimer() {
        // 写字机需长期保持 Wi-Fi/Telnet 待命；三分钟后只降到仍能辨识的亮度，禁用自动断电。
        power_save_timer_ = new PowerSaveTimer(-1, 180, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(35);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    
    void InitializeTca9554(void)
    {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
        if(ret != ESP_OK)
        ESP_LOGE(TAG, "TCA9554 create returned error");        
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);         
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 1);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        // WAVESHARE 3.5 若无 AXP2101 PMIC 则探测不到 ACK，缺失则跳过。
        // 用 i2c_master_probe（吃 bus handle）而非 i2c_master_transmit_receive
        //（吃 device handle），与全仓 35 处板级探测用法一致。
        esp_err_t ret = i2c_master_probe(i2c_bus_, 0x34, 100);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "no AXP2101 on I2C, skipping PMIC init");
            return;
        }
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MISO_PIN;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }
    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAM_PIN_D0,
                [1] = CAM_PIN_D1,
                [2] = CAM_PIN_D2,
                [3] = CAM_PIN_D3,
                [4] = CAM_PIN_D4,
                [5] = CAM_PIN_D5,
                [6] = CAM_PIN_D6,
                [7] = CAM_PIN_D7,
            },
            .vsync_io = CAM_PIN_VSYNC,
            .de_io = CAM_PIN_HREF,
            .pclk_io = CAM_PIN_PCLK,
            .xclk_io = CAM_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,  // 不初始化新的 SCCB，使用现有的 I2C 总线
            .i2c_handle = i2c_bus_,  // 使用现有的 I2C 总线句柄
            .freq = 100000,  // 100kHz
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAM_PIN_RESET,
            .pwdn_pin = CAM_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = 12000000,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
        
    }

    void InitializeTouch() {
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
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        constexpr int kFt6x36ThresholdReg = 0x80;
        constexpr int kFt6x36ActivePeriodReg = 0x88;
        constexpr int kFt6x36ChipIdReg = 0xA3;
        constexpr int kFt6x36VendorIdReg = 0xA8;
        uint8_t threshold = 0;
        // FT5x06 0x80 TH_GROUP（实值 = 4×寄存器值，出厂 70 即 280）。40 仍漏
        // 轻触（2026-08-20 用户反馈不灵敏），降到 30（实值 120；Linux EDT
        // 驱动接受 20–80 区间，仍在官方有效范围）。灵敏度提高带来的静止抖动
        // 由下方 24px 滚动阈值与 24px 拖动阈值兜底。
        constexpr uint8_t kTouchThreshold = 30;
        uint8_t active_period = 0;
        uint8_t chip_id = 0;
        uint8_t vendor_id = 0;
        const esp_err_t threshold_err =
            esp_lcd_panel_io_rx_param(tp_io_handle, kFt6x36ThresholdReg, &threshold, 1);
        const esp_err_t period_err =
            esp_lcd_panel_io_rx_param(tp_io_handle, kFt6x36ActivePeriodReg, &active_period, 1);
        const esp_err_t chip_err =
            esp_lcd_panel_io_rx_param(tp_io_handle, kFt6x36ChipIdReg, &chip_id, 1);
        const esp_err_t vendor_err =
            esp_lcd_panel_io_rx_param(tp_io_handle, kFt6x36VendorIdReg, &vendor_id, 1);
        if (threshold_err == ESP_OK && period_err == ESP_OK && chip_err == ESP_OK &&
            vendor_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "Touch registers: threshold=%u active_period=%u chip=0x%02X vendor=0x%02X",
                     threshold, active_period, chip_id, vendor_id);
        } else {
            ESP_LOGW(TAG, "Touch register read failed: threshold=%s period=%s chip=%s vendor=%s",
                     esp_err_to_name(threshold_err), esp_err_to_name(period_err),
                     esp_err_to_name(chip_err), esp_err_to_name(vendor_err));
        }
        if (threshold_err == ESP_OK && threshold != kTouchThreshold) {
            const uint8_t old_threshold = threshold;
            const esp_err_t write_err =
                esp_lcd_panel_io_tx_param(tp_io_handle, kFt6x36ThresholdReg, &kTouchThreshold, 1);
            const esp_err_t verify_err =
                write_err == ESP_OK
                    ? esp_lcd_panel_io_rx_param(tp_io_handle, kFt6x36ThresholdReg, &threshold, 1)
                    : write_err;
            if (verify_err == ESP_OK && threshold == kTouchThreshold) {
                ESP_LOGI(TAG, "Touch threshold tuned: %u -> %u", old_threshold, threshold);
            } else {
                ESP_LOGW(TAG, "Touch threshold tune failed: write=%s verify=%s value=%u",
                         esp_err_to_name(write_err), esp_err_to_name(verify_err), threshold);
            }
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lv_indev_t* touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (touch_indev != nullptr) {
            // 本板 FT5x06 阈值已调敏（上方 70->30），静止按压抖动超过 LVGL 默认 10px
            // 滚动阈值（LV_INDEV_DEF_SCROLL_LIMIT，lv_indev.c:1375 越限即 PRESS_LOST）：
            // 点按被误判成拖动、CLICKED 全数被吃（2026-08-20 HIL 坐实：绘图机抽屉
            // 展开态按钮几乎全哑）。提到 24px 与抽屉触发钮点按阈值（lcd_display.cc
            // kTriggerDragThresholdPx）同量级：抖动不触发滚动，明确拖动仍可经
            // SCROLL_CHAIN 滚面板。断 chain 的替代方案会让面板无处起手滚动，已否决。
            lv_indev_set_scroll_limit(touch_indev, 24);
            // LV_DEF_REFR_PERIOD=33（sdkconfig 实测）：indev 读定时器默认 33ms 才
            // 采一次触摸，是「不跟手」的主延迟源（LVGL issue #8152 同因）。FT6336
            // INT 未接 GPIO 只能轮询，显式提到 10ms 一读——400kHz I2C 读状态+坐标
            // 几字节约 0.2ms，CPU 代价可忽略。芯片侧 0x88 主动采样周期 12 已是官方
            // 文档最小推荐值（FT5x06_registers.pdf「should not less than 12」），
            // 无空间再降。
            lv_timer_set_period(lv_indev_get_read_timer(touch_indev), 10);
            lv_indev_add_event_cb(
                touch_indev,
                [](lv_event_t* event) {
                    auto* timer = static_cast<PowerSaveTimer*>(lv_event_get_user_data(event));
                    timer->WakeUp();
                },

                LV_EVENT_PRESSED, power_save_timer_);
        }
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        st7796_vendor_config_t st7796_vendor_config = {
            .init_cmds = st7796_lcd_init_cmds,
            .init_cmds_size = sizeof(st7796_lcd_init_cmds) / sizeof(st7796_lcd_init_cmd_t),
        };      

        // 初始化液晶屏驱动芯片
        ESP_LOGI(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &st7796_vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
 
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // 初始化工具
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

        // hutuji 写字机 Telnet 哑管道（TCP 客户端 → Grbl_Esp32 Telnet:23）。
        hutuji::Pipe::GetInstance().Start();

        // BLE-DIAG 阶段 A 只读诊断广播；默认关闭，未启用时是空实现。
        hutuji::ble_diag::Start();

        display_->ConfigureMachineControls(
            [this]() { ScheduleMachineControl("pause", &hutuji::Job::RequestPause); },
            [this]() { ScheduleMachineControl("resume", &hutuji::Job::RequestResume); },
            [this]() { ScheduleMachineControl("abort", &hutuji::Job::RequestAbort); },
            [this]() { ScheduleMachineControl("repeat", &hutuji::Job::RequestRepeat); },
            [this]() { ScheduleMachineControl("pen_test", &hutuji::Job::RequestPenTest); },
            [this](const char* action) { ScheduleManualControl(action); });

        // boot 键功能上屏：「说话」与 boot 单击完全同语义（starting 态转配网，
        // 否则 ToggleChatState）；触摸唤醒已由 LV_EVENT_PRESSED 钩子在板级完成，
        // 这里不再重复 WakeUp。「配网」直接进配网模式，屏显二维码。
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
        // TryWifiConnect（有凭据回连；无凭据新机按上游流程弹回配网）。必须
        // Schedule 回主循环：StopConfigAp 的事件回调是同步调用，新机无凭据时
        // TryWifiConnect 内部有 vTaskDelay(1500)，在 taskLVGL 上跑会卡死 UI。
        display_->SetProvisioningCancelHandler([]() {
            Application::GetInstance().Schedule(
                []() { WifiManager::GetInstance().StopConfigAp(); });
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
            "按钮，用户确认后才调 hutuji.confirm。",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("preview_url", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string& url = properties["url"].value<std::string>();
                const std::string& preview_url = properties["preview_url"].value<std::string>();
                return hutuji::Job::GetInstance().StartDraw(url, preview_url);
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
    // 同室部署写字机：10dBm(40=0.25dBm 单位) 足够覆盖，显著降低 Wi-Fi 峰值电流，
    // 与 Always PWM、UVP 240us 防抖共同缓解无电池 VSYS 的瞬态下陷。
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
                    StopWifiLostWatchdog();
                    const std::string ap_ssid = WifiManager::GetInstance().GetApSsid();
                    display_->ShowProvisioningQr(
                        hutuji::BuildOpenHotspotWifiQrPayload(ap_ssid),
                        "Scan: " + ap_ssid + "\nOpen: " + WifiManager::GetInstance().GetApWebUrl());
                } else if (event == NetworkEvent::WifiConfigModeExit ||
                           event == NetworkEvent::Connected) {
                    StopWifiLostWatchdog();
                    display_->HideProvisioningQr();
                } else if (event == NetworkEvent::Disconnected) {
                    StartWifiLostWatchdog();
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
                    auto* self = static_cast<CustomBoard*>(arg);
                    // 先原地续表再 Schedule：EnterWifiConfigMode 对 Connecting/配网中/
                    // 升级中等状态门控早退（wifi_board.cc 的 state 检查），one-shot 若
                    // 不重武装，AP 关停期间「断连 120s 自动显码」静默失效（2026-08-20
                    // 复审 P1-1）。成功进配网由 WifiConfigModeEnter 事件 Stop，连上由
                    // Connected 事件 Stop；esp_timer 回调上下文内 stop/start_once 合法。
                    self->StartWifiLostWatchdog();
                    // esp_timer 任务上下文不直接碰网络状态机：Schedule 回主循环执行。
                    Application::GetInstance().Schedule([self]() {
                        if (WifiManager::GetInstance().IsConnected()) {
                            // 起到主循环执行的间隙里已恢复（Connected 事件会停表），
                            // 不把已连上的设备踹进配网。
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
                // 安全网静默缺失比日志噪音更糟：alloc 失败时 esp_timer 自身不打日志。
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
    CustomBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeTca9554();
        InitializeAxp2101();
        InitializeSpi();
        InitializeLcdDisplay();
        // 解决部分开机黑屏的问题
        if (esp_reset_reason() == ESP_RST_POWERON) {
            fflush(stdout);
            esp_restart();
        }
        InitializeTouch();
        InitializeButtons();
        InitializeCamera();
        // 无电池缓冲（PMIC status1 无电池位）时，NS4150B 功放是 VSYS 最大瞬态负载；
        // 音量直接线性放大播报峰值电流。启动时把音量钳到 50，用户仍可语音再调。
        auto* codec = GetAudioCodec();
        if (codec->output_volume() > 50) {
            codec->SetOutputVolume(50);
        }
        InitializeTools();
        GetBacklight()->RestoreBrightness();
        // USB CDC 枚举较晚，15 秒后复述同一份启动锁存值，不重新读寄存器。
        ReplayPmicBootStatusAfterUsbReady();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CustomBoard);
