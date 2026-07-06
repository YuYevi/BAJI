#include "audio_debugger.h"
#include "sdkconfig.h"

#if CONFIG_USE_AUDIO_DEBUGGER
#include <esp_log.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <string>
#endif

#define TAG "AudioDebugger"


AudioDebugger::AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    udp_sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sockfd_ >= 0) {
        
        std::string server_addr = CONFIG_AUDIO_DEBUG_UDP_SERVER;
        size_t colon_pos = server_addr.find(':');
        
        if (colon_pos != std::string::npos) {
            std::string ip = server_addr.substr(0, colon_pos);
            int port = std::stoi(server_addr.substr(colon_pos + 1));
            
            memset(&udp_server_addr_, 0, sizeof(udp_server_addr_));
            udp_server_addr_.sin_family = AF_INET;
            udp_server_addr_.sin_port = htons(port);
            inet_pton(AF_INET, ip.c_str(), &udp_server_addr_.sin_addr);
            
            
        } else {
            
            close(udp_sockfd_);
            udp_sockfd_ = -1;
        }
    } else {
        
    }
#endif
}

AudioDebugger::~AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        close(udp_sockfd_);
        
    }
#endif
}

void AudioDebugger::Feed(const std::vector<int16_t>& data) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        ssize_t sent = sendto(udp_sockfd_, data.data(), data.size() * sizeof(int16_t), 0,
                             (struct sockaddr*)&udp_server_addr_, sizeof(udp_server_addr_));
        if (sent < 0) {
            
        } else {
            ESP_LOGD(TAG, "Sent %d bytes audio data to %s", sent, CONFIG_AUDIO_DEBUG_UDP_SERVER);
        }
    }
#endif
}

 