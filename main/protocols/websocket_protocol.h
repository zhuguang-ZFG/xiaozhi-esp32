#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>
#include <memory>
#include <mutex>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define WEBSOCKET_PROTOCOL_CLOSED_EVENT (1 << 1)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    struct ChannelState;
    // 锁也由通道持有：hello 等待者在 owner 析构后仍可安全检查退休状态。
    std::shared_ptr<std::mutex> channel_mutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<ChannelState> channel_;
    std::shared_ptr<WebSocket> websocket_;
    int version_ = 1;

    static void Receive(const std::shared_ptr<ChannelState>& state, const char* data, size_t len,
                        bool binary);
    static void ParseServerHello(const std::shared_ptr<ChannelState>& state, const cJSON* root);
    static bool FailChannel(WebsocketProtocol* owner, const std::shared_ptr<ChannelState>& state,
                            std::shared_ptr<WebSocket> websocket, const std::string& error);
    bool SendText(const std::string& text) override;
    bool IsTimeout() const override;
    static std::string GetHelloMessage(int version);
};

#endif
