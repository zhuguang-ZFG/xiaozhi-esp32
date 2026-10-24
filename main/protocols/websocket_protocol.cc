#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WS"

struct WebsocketProtocol::ChannelState {
    explicit ChannelState(std::shared_ptr<std::mutex> lock, int protocol_version)
        : mutex(std::move(lock)), version(protocol_version), events(xEventGroupCreate()) {}
    ~ChannelState() { vEventGroupDelete(events); }

    // 调用者持短锁；通知先领取再锁外调用，远端回调不拥有或析构 transport。
    std::function<void()> RetireLocked() {
        retired = true;
        xEventGroupSetBits(events, WEBSOCKET_PROTOCOL_CLOSED_EVENT);
        if (close_notified)
            return {};
        close_notified = true;
        return std::move(on_closed);
    }

    bool IsTimeoutLocked() const {
        // 保持既有整秒取整及严格超过 120 秒的规则。
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                last_incoming)
                   .count() > 120;
    }

    const std::shared_ptr<std::mutex> mutex;
    const int version;
    const EventGroupHandle_t events;
    bool retired = false;
    // 远端退休不等于 owner 析构；等待者可据此在所属任务回收残留传输。
    bool attached = false;
    bool close_notified = false;
    int sample_rate = 24000;
    int frame_duration = 60;
    std::string session_id;
    std::chrono::steady_clock::time_point last_incoming;
    std::function<void(std::unique_ptr<AudioStreamPacket>)> on_audio;
    std::function<void(const cJSON*)> on_json;
    std::function<void()> on_opened;
    std::function<void()> on_closed;
    std::function<void(const std::string&)> on_error;
};

WebsocketProtocol::WebsocketProtocol() = default;

namespace {
// F50-11：接收任务卡死时 ~WebSocket 的有限等待最长 ~30s；失败/替换/关闭路径
// 不得把音频编码任务或主循环拖在旧对象的同步析构上。由一次性回收任务持有
// 强引用完成销毁；任务创建失败才退回同步析构（有界等待语义不变）。
void RetireWebSocketAsync(std::shared_ptr<WebSocket> websocket) {
    if (!websocket)
        return;
    auto* holder = new std::shared_ptr<WebSocket>(std::move(websocket));
    if (xTaskCreate([](void* arg) {
            delete static_cast<std::shared_ptr<WebSocket>*>(arg);
            vTaskDelete(nullptr);
        }, "ws_retire", 4096, holder, 3, nullptr) != pdPASS) {
        delete holder;
    }
}
}  // namespace

WebsocketProtocol::~WebsocketProtocol() { CloseAudioChannel(); }

bool WebsocketProtocol::Start() {
    // 只在需要音频通道时连接。
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::shared_ptr<ChannelState> state;
    std::shared_ptr<WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(*channel_mutex_);
        state = channel_;
        websocket = websocket_;
        if (!state || state->retired || !websocket)
            return false;
    }
    if (!websocket->IsConnected())
        return false;

    bool sent;
    if (state->version == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(state->version);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());
        sent = websocket->Send(serialized.data(), serialized.size(), true);
    } else if (state->version == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());
        sent = websocket->Send(serialized.data(), serialized.size(), true);
    } else {
        sent = websocket->Send(packet->payload.data(), packet->payload.size(), true);
    }
    if (!sent)
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_ERROR);
    return true;
}

bool WebsocketProtocol::SendText(const std::string& text) {
    std::shared_ptr<ChannelState> state;
    std::shared_ptr<WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(*channel_mutex_);
        state = channel_;
        websocket = websocket_;
        if (!state || state->retired || !websocket)
            return false;
    }
    if (!websocket->IsConnected())
        return false;
    if (!websocket->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text");
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_ERROR);
    }
    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    std::shared_ptr<WebSocket> websocket;
    bool active;
    {
        std::lock_guard<std::mutex> lock(*channel_mutex_);
        websocket = websocket_;
        active = channel_ && !channel_->retired && !error_occurred_ && !channel_->IsTimeoutLocked();
    }
    return active && websocket && websocket->IsConnected();
}

bool WebsocketProtocol::IsTimeout() const {
    std::lock_guard<std::mutex> lock(*channel_mutex_);
    return !channel_ || channel_->IsTimeoutLocked();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    auto mutex = channel_mutex_;
    std::shared_ptr<ChannelState> state;
    std::shared_ptr<WebSocket> websocket;
    std::function<void()> closed;
    {
        std::lock_guard<std::mutex> lock(*mutex);
        state = std::move(channel_);
        websocket = std::move(websocket_);
        if (state) {
            state->attached = false;
            closed = state->RetireLocked();
        }
    }
    // 通知可能重入并销毁协议，以下只使用局部所有权，不再访问 this。
    RetireWebSocketAsync(std::move(websocket));
    if (closed)
        closed();
}

bool WebsocketProtocol::FailChannel(WebsocketProtocol* owner,
                                    const std::shared_ptr<ChannelState>& state,
                                    std::shared_ptr<WebSocket> websocket,
                                    const std::string& error) {
    std::function<void()> closed;
    std::function<void(const std::string&)> failed;
    {
        std::lock_guard<std::mutex> lock(*state->mutex);
        // 与析构/替换同锁；脱离后 owner 可能已不存在，不能解引用。
        if (state->attached) {
            if (!state->retired) {
                owner->error_occurred_ = true;
                failed = state->on_error;
            }
            state->attached = false;
            owner->websocket_.reset();  // websocket 局部强引用保证不在锁内析构。
            owner->channel_.reset();
            closed = state->RetireLocked();
        }
    }
    RetireWebSocketAsync(std::move(websocket));
    if (failed)
        failed(error);
    if (closed)
        closed();
    return false;
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    auto mutex = channel_mutex_;
    std::shared_ptr<ChannelState> previous;
    std::shared_ptr<WebSocket> previous_websocket;
    std::function<void()> previous_closed;
    std::shared_ptr<ChannelState> state;
    std::shared_ptr<WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(*mutex);
        previous = std::move(channel_);
        previous_websocket = std::move(websocket_);
        if (previous) {
            previous->attached = false;
            previous_closed = previous->RetireLocked();
        }
        // F50-02 评估结论（2026-10-24）：不得放宽为「通知已消费即本回合新建」。
        // 关闭通知被消费后排入的 Idle 仍可能晚于本回合执行（回归
        // replace_batch_remote_race 钉住该交错），此时报告新通道成功会被那条
        // Idle 覆盖 Listening。正确修法在应用层（关闭回调派生的 Idle 须带通道
        // 代次守卫），需上板验证后再动；协议层维持本回合退休+失败。
        if (!previous) {
            if (version == 0)
                version = version_;
            state = std::make_shared<ChannelState>(mutex, version);
            auto network = Board::GetInstance().GetNetwork();
            websocket = network->CreateWebSocket(1);
            state->on_audio = on_incoming_audio_;
            state->on_json = on_incoming_json_;
            state->on_opened = on_audio_channel_opened_;
            state->on_closed = on_audio_channel_closed_;
            state->on_error = on_network_error_;
            channel_ = state;
            websocket_ = websocket;
            version_ = version;
            error_occurred_ = false;
            state->attached = true;
        }
    }
    // 旧 websocket 的析构可能因接收者卡死等待最长 ~30s；异步回收，不占本回合。
    RetireWebSocketAsync(std::move(previous_websocket));
    if (previous) {
        if (previous_closed)
            previous_closed();
        return false;  // 通知可能销毁协议，此后不得访问 this。
    }
    bool retired;
    {
        std::lock_guard<std::mutex> lock(*mutex);
        retired = state->retired;
    }
    if (retired)
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_NOT_CONNECTED);
    if (!websocket) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_NOT_CONNECTED);
    }
    auto message = GetHelloMessage(version);

    if (!token.empty()) {
        if (token.find(" ") == std::string::npos)
            token = "Bearer " + token;
        websocket->SetHeader("Authorization", token.c_str());
    }
    websocket->SetHeader("Protocol-Version", std::to_string(version).c_str());
    websocket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    std::weak_ptr<ChannelState> weak = state;
    websocket->OnData([weak](const char* data, size_t len, bool binary) {
        if (auto state = weak.lock())
            Receive(state, data, len, binary);
    });
    websocket->OnDisconnected([weak]() {
        if (auto state = weak.lock()) {
            std::function<void()> closed;
            {
                std::lock_guard<std::mutex> lock(*state->mutex);
                closed = state->RetireLocked();
            }
            ESP_LOGI(TAG, "Websocket disconnected");
            if (closed)
                closed();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version);
    if (!websocket->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket->GetLastError());
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_NOT_CONNECTED);
    }
    {
        std::lock_guard<std::mutex> lock(*mutex);
        retired = state->retired;
    }
    if (retired)
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_NOT_CONNECTED);
    if (!websocket->Send(message)) {
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_ERROR);
    }

    // 等待者持有本次 state；关闭与 hello 都会唤醒，旧连接不共享事件位。
    EventBits_t bits = xEventGroupWaitBits(
        state->events, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT | WEBSOCKET_PROTOCOL_CLOSED_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    std::function<void()> opened;
    {
        std::lock_guard<std::mutex> lock(*mutex);
        retired = state->retired;
        if (!retired && (bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
            server_sample_rate_ = state->sample_rate;
            server_frame_duration_ = state->frame_duration;
            session_id_ = state->session_id;
            opened = state->on_opened;
        }
    }
    if (retired)
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_NOT_CONNECTED);
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_TIMEOUT);
    }
    {
        // 解锁到 opened 之间远端可能已关闭；先复核，不要在已退休的会话上报告打开。
        std::lock_guard<std::mutex> lock(*mutex);
        retired = state->retired;
    }
    if (retired)
        return FailChannel(this, state, std::move(websocket), Lang::Strings::SERVER_NOT_CONNECTED);
    if (opened)
        opened();
    {
        // opened 回调期间关闭同理；只使用局部 state/mutex，回调可能已销毁协议。
        // 返回值在锁内取快照，解锁后不再读取共享状态。
        std::lock_guard<std::mutex> lock(*mutex);
        return !state->retired;
    }
}

void WebsocketProtocol::Receive(const std::shared_ptr<ChannelState>& state, const char* data,
                                size_t len, bool binary) {
    std::function<void(std::unique_ptr<AudioStreamPacket>)> audio;
    std::function<void(const cJSON*)> json;
    int sample_rate;
    int frame_duration;
    {
        std::lock_guard<std::mutex> lock(*state->mutex);
        if (state->retired)
            return;
        state->last_incoming = std::chrono::steady_clock::now();
        sample_rate = state->sample_rate;
        frame_duration = state->frame_duration;
        if (binary)
            audio = state->on_audio;
        else
            json = state->on_json;
    }
    // 已领取的调用可以完成，但只保留 state/参数/回调副本；解析和下游均锁外。
    if (binary) {
        if (!audio)
            return;
        size_t header_size = 0;
        size_t payload_size = len;
        uint32_t timestamp = 0;
        if (state->version == 2) {
            if (len < sizeof(BinaryProtocol2))
                return;
            BinaryProtocol2 header;
            memcpy(&header, data, sizeof(header));
            header_size = sizeof(header);
            payload_size = ntohl(header.payload_size);
            timestamp = ntohl(header.timestamp);
        } else if (state->version == 3) {
            if (len < sizeof(BinaryProtocol3))
                return;
            BinaryProtocol3 header;
            memcpy(&header, data, sizeof(header));
            header_size = sizeof(header);
            payload_size = ntohs(header.payload_size);
        }
        if (payload_size > len - header_size)
            return;
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration;
        packet->timestamp = timestamp;
        if (payload_size) {
            auto payload = reinterpret_cast<const uint8_t*>(data) + header_size;
            packet->payload.assign(payload, payload + payload_size);
        }
        audio(std::move(packet));
    } else {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(data, len),
                                                             cJSON_Delete);
        auto type = cJSON_GetObjectItem(root.get(), "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "hello") == 0)
                ParseServerHello(state, root.get());
            else if (json)
                json(root.get());
        } else {
            ESP_LOGE(TAG, "Missing message type");
        }
    }
}

std::string WebsocketProtocol::GetHelloMessage(int version) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const std::shared_ptr<ChannelState>& state,
                                         const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport");
        return;
    }
    auto session_id = cJSON_GetObjectItem(root, "session_id");
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
    auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
    std::lock_guard<std::mutex> lock(*state->mutex);
    if (state->retired)
        return;
    if (cJSON_IsString(session_id))
        state->session_id = session_id->valuestring;
    if (cJSON_IsNumber(sample_rate))
        state->sample_rate = sample_rate->valueint;
    if (cJSON_IsNumber(frame_duration))
        state->frame_duration = frame_duration->valueint;
    xEventGroupSetBits(state->events, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
