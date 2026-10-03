#pragma once

#include <stdint.h>

#include <string>

#include <microReticulum/Interface.h>

#include "solar_os_reticulum_hdlc.h"

namespace solar_os {

// Reticulum TCPClientInterface: HDLC-framed packets over one outbound TCP
// connection. The owner drives it through Transport, which calls loop().
class ReticulumTcpInterface : public RNS::InterfaceImpl {
public:
    ReticulumTcpInterface(const char *host, uint16_t port);
    ~ReticulumTcpInterface() override;

    uint32_t connects() const { return connects_; }

protected:
    bool start() override;
    void stop() override;
    void loop() override;
    bool send_outgoing(const RNS::Bytes &data) override;

private:
    enum class State { Idle, Connecting, Connected };

    void begin_connect();
    void finish_connect();
    void close_socket();
    void read_available();
    static void on_frame(const uint8_t *frame, size_t length, void *context);

    std::string host_;
    uint16_t port_;
    int socket_ = -1;
    State state_ = State::Idle;
    bool running_ = false;
    uint32_t retry_delay_ms_ = 0;
    uint32_t next_attempt_ms_ = 0;
    uint32_t connect_started_ms_ = 0;
    uint32_t connects_ = 0;
    solar_os_reticulum_hdlc_decoder_t decoder_;
    uint8_t decode_buffer_[1200];
};

}  // namespace solar_os
