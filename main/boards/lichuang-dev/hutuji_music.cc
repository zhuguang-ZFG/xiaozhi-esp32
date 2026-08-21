#include "hutuji_music.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "display.h"
#include "http.h"
#include "ogg_demuxer.h"

namespace hutuji {

namespace {
constexpr const char* kTag = "hutuji_music";
// 与 hutuji_job 下载任务同量级：TLS 握手是栈大头，不能按纯投喂循环估。
constexpr uint32_t kTaskStackBytes = 8192;
}  // namespace

HutujiMusic& HutujiMusic::GetInstance() {
    static HutujiMusic instance;
    return instance;
}

void HutujiMusic::TransitionTo(music::MusicState next) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (music::CanTransition(state_, next)) {
        state_ = next;
    } else {
        ESP_LOGW(kTag, "非法状态转移 %d -> %d，忽略", static_cast<int>(state_),
                 static_cast<int>(next));
    }
}

void HutujiMusic::SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
    ESP_LOGW(kTag, "%s", error.c_str());
}

bool HutujiMusic::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ != music::MusicState::kIdle;
}

std::string HutujiMusic::CurrentTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_;
}

std::string HutujiMusic::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool HutujiMusic::DeviceStateAllowsMusic() {
    // 音乐在 idle/speaking/listening 都成立：会话开着时设备就停在 listening
    // （喇叭此时空闲，AEC 以播放为参考，歌声不会误唤醒）；2026-08-21 实机
    // 实测点歌后下载完成的瞬间设备必在 listening，把它当「忙」会让真唱永远
    // 被掐死。真正的让位条件是链路重建类状态（connecting/activating/upgrade），
    // 用户喊停走云端 hutuji.stop_song。
    const DeviceState state = Application::GetInstance().GetDeviceState();
    return state == kDeviceStateIdle || state == kDeviceStateSpeaking ||
           state == kDeviceStateListening;
}

bool HutujiMusic::Play(const std::string& url, const std::string& title) {
    if (!music::IsValidSongUrl(url)) {
        SetError("URL 不在歌曲白名单");
        return false;
    }
    if (IsActive()) {
        ESP_LOGI(kTag, "切歌：先停上一首");
        Stop();
        if (!WaitStopped(5000)) {
            SetError("上一首未能在 5s 内退出");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = url;
        title_ = title.substr(0, music::kMaxTitleChars);
        last_error_.clear();
        buffer_ = nullptr;
        buffer_len_ = 0;
    }
    stop_requested_.store(false);
    TransitionTo(music::MusicState::kDownloading);

    if (xTaskCreate(TaskEntry, "hutuji_music", kTaskStackBytes, this, 4, nullptr) != pdPASS) {
        stop_requested_.store(true);
        TransitionTo(music::MusicState::kStopping);
        TransitionTo(music::MusicState::kIdle);
        SetError("任务创建失败");
        return false;
    }
    ESP_LOGI(kTag, "开始播放：%s", title_.c_str());
    return true;
}

void HutujiMusic::Stop() {
    if (!IsActive()) {
        return;
    }
    stop_requested_.store(true);
    TransitionTo(music::MusicState::kStopping);
    // 即刻静音：清掉已入队的本曲包（也会清掉正在说的 TTS，喊停就该立刻安静）。
    Application::GetInstance().GetAudioService().ResetDecoder();
}

bool HutujiMusic::WaitStopped(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (IsActive()) {
        if (xTaskGetTickCount() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void HutujiMusic::TaskEntry(void* arg) {
    auto* self = static_cast<HutujiMusic*>(arg);
    self->Run();
    vTaskDelete(nullptr);
}

void HutujiMusic::Run() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
    }

    if (!DownloadToPsram(url)) {
        // 错误已写 last_error_
    } else if (stop_requested_.load()) {
        // 下载完成后用户喊停：静默退场，不算错误
    } else if (!DeviceStateAllowsMusic()) {
        SetError("设备忙，放弃播放");
    } else {
        uint32_t got = Crc32Ieee(buffer_, buffer_len_);
        if (!Crc32Matches(expect_crc_, got)) {
            SetError("CRC 不符");
        } else {
            TransitionTo(music::MusicState::kPlaying);
            PumpDecoded();
        }
    }

    if (buffer_ != nullptr) {
        heap_caps_free(buffer_);
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_ = nullptr;
        buffer_len_ = 0;
    }

    // 统一退场：kDownloading/kPlaying 可能直接转 kIdle，也可能先到 kStopping。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == music::MusicState::kDownloading || state_ == music::MusicState::kPlaying) {
            state_ = music::MusicState::kIdle;
        } else if (state_ == music::MusicState::kStopping) {
            state_ = music::MusicState::kIdle;
        }
    }
    ESP_LOGI(kTag, "播放任务退出");
}

bool HutujiMusic::DownloadToPsram(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        SetError("无网络");
        return false;
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        SetError("CreateHttp 失败");
        return false;
    }
    http->SetTimeout(60000);
    // 生产域由 IsValidSongUrl 强制 HTTPS；HTTP 只允许 RFC1918 联调主机。
    if (!http->Open("GET", url)) {
        SetError("HTTP Open 失败");
        return false;
    }
    if (http->GetStatusCode() != 200) {
        SetError("HTTP status " + std::to_string(http->GetStatusCode()));
        http->Close();
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > music::kMaxSongBytes) {
        SetError("歌曲长度非法");
        http->Close();
        return false;
    }

    std::string crc_hdr = http->GetResponseHeader("X-Hutuji-CRC32");
    if (crc_hdr.empty()) {
        crc_hdr = http->GetResponseHeader("x-hutuji-crc32");
    }
    if (!ParseCrc32Header(crc_hdr, expect_crc_)) {
        SetError(crc_hdr.empty() ? "缺少 X-Hutuji-CRC32" : "X-Hutuji-CRC32 格式无效");
        http->Close();
        return false;
    }

    // 只用 PSRAM，不回落内部 RAM（与 hutuji_job 同理由：fail closed 更可诊断）。
    uint8_t* buffer = static_cast<uint8_t*>(
        heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        SetError("PSRAM 分配失败");
        http->Close();
        return false;
    }

    size_t total = 0;
    bool ok = true;
    while (total < content_length) {
        if (stop_requested_.load()) {
            ok = false;
            break;
        }
        int n = http->Read(reinterpret_cast<char*>(buffer + total), content_length - total);
        if (n < 0) {
            SetError("HTTP Read 失败");
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<size_t>(n);
    }
    http->Close();

    if (!ok) {
        heap_caps_free(buffer);
        return false;
    }
    if (total != content_length) {
        heap_caps_free(buffer);
        SetError("长度不符 expect=" + std::to_string(content_length) +
                 " got=" + std::to_string(total));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    buffer_ = buffer;
    buffer_len_ = total;
    ESP_LOGI(kTag, "歌曲下载完成 %u 字节", static_cast<unsigned>(buffer_len_));
    return true;
}

void HutujiMusic::PumpDecoded() {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        SetError("无音频编解码器");
        return;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    {
        std::string notice = "♪ " + CurrentTitle();
        Display* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(notice.c_str(), 5000);
        }
    }

    auto& audio_service = Application::GetInstance().GetAudioService();
    auto demuxer = std::make_unique<OggDemuxer>();
    std::atomic<bool> push_failed{false};
    demuxer->OnDemuxerFinished(
        [this, &audio_service, &push_failed](const uint8_t* data, int sample_rate, size_t len) {
            if (stop_requested_.load() || !DeviceStateAllowsMusic()) {
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->payload.assign(data, data + len);
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            // wait=true 提供背压：投喂速度被解码消费钳住，PSRAM 缓冲不会撑爆队列。
            if (!audio_service.PushPacketToDecodeQueue(std::move(packet), true)) {
                push_failed.store(true);
            }
        });

    size_t offset = 0;
    while (offset < buffer_len_) {
        if (stop_requested_.load() || push_failed.load() || !DeviceStateAllowsMusic()) {
            break;
        }
        const size_t chunk = (buffer_len_ - offset < music::kDemuxChunkBytes)
                                 ? buffer_len_ - offset
                                 : music::kDemuxChunkBytes;
        demuxer->Process(buffer_ + offset, chunk);
        offset += chunk;
    }

    if (stop_requested_.load()) {
        ESP_LOGI(kTag, "播放被停止");
    } else if (!DeviceStateAllowsMusic()) {
        ESP_LOGI(kTag, "设备状态变化，停止播放");
    } else {
        ESP_LOGI(kTag, "播放完成");
    }
}

}  // namespace hutuji
