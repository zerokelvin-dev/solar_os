#pragma once

#include <stdint.h>

#include <microReticulum/Interface.h>

extern "C" {
#include "solar_os_radio.h"
#include "solar_os_reticulum_lora_frame.h"
}

namespace solar_os {

// A Reticulum interface over a claimed packet radio, framed the way an RNode
// frames the air, so a stock RNode is a peer. The owner claims, configures
// and releases the radio; this drives it. Transport calls loop().
class ReticulumLoraInterface : public RNS::InterfaceImpl {
public:
    ReticulumLoraInterface(const solar_os_radio_handle_t &handle,
                           const solar_os_radio_config_t &config);

    uint32_t send_errors() const { return send_errors_; }
    uint32_t crc_errors() const { return crc_errors_; }
    bool heard() const { return heard_; }
    int16_t last_rssi_dbm() const { return last_rssi_; }
    int16_t last_snr_db() const { return last_snr_; }

protected:
    bool start() override;
    void stop() override;
    void loop() override;
    bool send_outgoing(const RNS::Bytes &data) override;

private:
    uint32_t airtime_ms(size_t length) const;
    void back_to_receive();

    solar_os_radio_handle_t handle_;
    solar_os_radio_config_t config_;
    solar_os_rnode_assembler_t assembler_;
    uint8_t packet_[SOLAR_OS_RNODE_MTU];
    uint32_t send_errors_ = 0;
    uint32_t crc_errors_ = 0;
    bool heard_ = false;
    int16_t last_rssi_ = 0;
    int16_t last_snr_ = 0;
};

}  // namespace solar_os
