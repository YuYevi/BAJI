#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>
#include <sys/select.h>
#include <unistd.h>

static const char *TAG = "EspSsl";

namespace {
constexpr int kTlsConnectTimeoutMs = 30000;
}

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspSsl::Connect(const std::string& host, int port) {
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return false;
    }

    disconnect_requested_.store(false);

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kTlsConnectTimeoutMs;

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        esp_tls_error_handle_t last_error;
        if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t err = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
            last_error_ = err;
            ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, err);
        } else {
            last_error_ = -1;
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    xTaskCreate([](void* arg) {
        EspSsl* ssl = (EspSsl*)arg;
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    return true;
}

void EspSsl::Disconnect() {
    // Closing the socket wakes ReceiveTask with a read error. Mark this as an
    // expected shutdown so it is not reported as a transport failure.
    disconnect_requested_.store(true);
    connected_ = false;
    
    // Close socket if it is open
    if (tls_client_ != nullptr) {
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            // This should only be needed if the receive task is stuck outside
            // select(). Keep the force-close fallback to avoid leaking TLS state.
            int sockfd;
            ESP_ERROR_CHECK(esp_tls_get_conn_sockfd(tls_client_, &sockfd));
            if (sockfd >= 0) {
                close(sockfd);
            }
            bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT,
                                       pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        }
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }

        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    
    while (total_sent < data_size) {
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }

        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        
        total_sent += ret;
    }
    
    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    bool wait_for_socket = true;
    while (connected_) {
        if (wait_for_socket) {
            int sockfd;
            if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) != ESP_OK || sockfd < 0) {
                if (!disconnect_requested_.load()) {
                    ESP_LOGE(TAG, "Failed to get TLS socket for receive");
                }
                break;
            }

            // Keep the read task interruptible so Disconnect() does not have to
            // close the socket while esp_tls_conn_read() is using it.
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sockfd, &read_fds);
            struct timeval timeout = {};
            timeout.tv_usec = 100 * 1000;
            int ready = select(sockfd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (!connected_) {
                break;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ESP_LOGE(TAG, "TLS socket select failed: errno=%d", errno);
                break;
            }
            if (ready == 0) {
                continue;
            }
        }

        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());

        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            wait_for_socket = true;
            continue;
        }

        if (ret <= 0) {
            const bool expected_disconnect = disconnect_requested_.load();
            if (ret < 0 && !expected_disconnect) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            }
            connected_ = false;
            // 接收失败或连接断开时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }
        
        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
        wait_for_socket = false;
    }
}

int EspSsl::GetLastError() {
    return last_error_;
}
