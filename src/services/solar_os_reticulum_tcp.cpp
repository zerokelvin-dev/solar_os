#include "solar_os_reticulum_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <memory>

#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_timer.h"

namespace solar_os {

namespace {

constexpr uint32_t kRetryMinMs = 2000U;
constexpr uint32_t kRetryMaxMs = 60000U;
constexpr uint32_t kConnectTimeoutMs = 10000U;
constexpr int kSendWaitMs = 250;
constexpr uint32_t kBitrateGuess = 10U * 1000U * 1000U;

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

}  // namespace

ReticulumTcpInterface::ReticulumTcpInterface(const char *host, uint16_t port)
    : RNS::InterfaceImpl("TCPInterface"), host_(host), port_(port)
{
    _IN = true;
    _OUT = true;
    _bitrate = kBitrateGuess;
    _HW_MTU = RNS::Type::Reticulum::MTU;
    solar_os_reticulum_hdlc_decoder_init(
        &decoder_, decode_buffer_, sizeof(decode_buffer_));
}

ReticulumTcpInterface::~ReticulumTcpInterface()
{
    close_socket();
}

bool ReticulumTcpInterface::start()
{
    running_ = true;
    retry_delay_ms_ = kRetryMinMs;
    next_attempt_ms_ = now_ms();
    return true;
}

void ReticulumTcpInterface::stop()
{
    running_ = false;
    close_socket();
}

void ReticulumTcpInterface::close_socket()
{
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    state_ = State::Idle;
    _online = false;
    solar_os_reticulum_hdlc_decoder_init(
        &decoder_, decode_buffer_, sizeof(decode_buffer_));
}

void ReticulumTcpInterface::begin_connect()
{
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    char service[8];
    snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port_));
    if (getaddrinfo(host_.c_str(), service, &hints, &result) != 0 ||
        result == nullptr) {
        return;
    }
    const int fd = ::socket(result->ai_family, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        freeaddrinfo(result);
        return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int enabled = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));

    const int rc = ::connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return;
    }
    socket_ = fd;
    state_ = State::Connecting;
    connect_started_ms_ = now_ms();
}

void ReticulumTcpInterface::finish_connect()
{
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket_, &writable);
    struct timeval zero = {0, 0};
    if (select(socket_ + 1, nullptr, &writable, nullptr, &zero) > 0) {
        int error = 0;
        socklen_t length = sizeof(error);
        getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &length);
        if (error == 0) {
            state_ = State::Connected;
            _online = true;
            connects_++;
            retry_delay_ms_ = kRetryMinMs;
            return;
        }
        close_socket();
    } else if (now_ms() - connect_started_ms_ > kConnectTimeoutMs) {
        close_socket();
    }
}

void ReticulumTcpInterface::on_frame(const uint8_t *frame,
                                     size_t length,
                                     void *context)
{
    auto *self = static_cast<ReticulumTcpInterface *>(context);
    self->handle_incoming(RNS::Bytes(frame, length));
}

void ReticulumTcpInterface::read_available()
{
    uint8_t chunk[256];
    for (int budget = 0; budget < 8 && state_ == State::Connected; budget++) {
        const int received = ::recv(socket_, chunk, sizeof(chunk), 0);
        if (received > 0) {
            solar_os_reticulum_hdlc_decoder_feed(
                &decoder_, chunk, static_cast<size_t>(received),
                &ReticulumTcpInterface::on_frame, this);
            continue;
        }
        if (received == 0 ||
            (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)) {
            close_socket();
        }
        return;
    }
}

void ReticulumTcpInterface::loop()
{
    if (!running_) {
        return;
    }
    switch (state_) {
    case State::Idle: {
        if (static_cast<int32_t>(now_ms() - next_attempt_ms_) < 0) {
            return;
        }
        begin_connect();
        if (state_ == State::Idle) {
            next_attempt_ms_ = now_ms() + retry_delay_ms_;
            retry_delay_ms_ = retry_delay_ms_ * 2U > kRetryMaxMs
                ? kRetryMaxMs : retry_delay_ms_ * 2U;
        }
        return;
    }
    case State::Connecting:
        finish_connect();
        if (state_ == State::Idle) {
            next_attempt_ms_ = now_ms() + retry_delay_ms_;
        }
        return;
    case State::Connected:
        read_available();
        if (state_ == State::Idle) {
            next_attempt_ms_ = now_ms() + kRetryMinMs;
        }
        return;
    }
}

bool ReticulumTcpInterface::send_outgoing(const RNS::Bytes &data)
{
    if (state_ != State::Connected || data.size() == 0U) {
        return false;
    }
    const size_t capacity = SOLAR_OS_RETICULUM_HDLC_ENCODED_MAX(data.size());
    std::unique_ptr<uint8_t[]> wire(new uint8_t[capacity]);
    const size_t length = solar_os_reticulum_hdlc_encode(
        data.data(), data.size(), wire.get(), capacity);
    size_t sent = 0U;
    while (sent < length) {
        const int written = ::send(socket_, wire.get() + sent, length - sent, 0);
        if (written > 0) {
            sent += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket_, &writable);
            struct timeval wait = {0, kSendWaitMs * 1000};
            if (select(socket_ + 1, nullptr, &writable, nullptr, &wait) > 0) {
                continue;
            }
        }
        close_socket();
        return false;
    }
    InterfaceImpl::handle_outgoing(data);
    return true;
}

}  // namespace solar_os
