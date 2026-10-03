#include "solar_os_reticulum_lora.h"

#include <math.h>
#include <string.h>

#include <algorithm>

#include "esp_random.h"

namespace solar_os {

namespace {

constexpr uint32_t kSendTimeoutFloorMs = 1500U;
constexpr int kReceiveBudget = 4;

}  // namespace

ReticulumLoraInterface::ReticulumLoraInterface(
    const solar_os_radio_handle_t &handle,
    const solar_os_radio_config_t &config)
    : RNS::InterfaceImpl("LoRaInterface"), handle_(handle), config_(config)
{
    _IN = true;
    _OUT = true;
    _HW_MTU = RNS::Type::Reticulum::MTU;
    /* As the RNode interface reckons it: symbol rate times bits per symbol
     * times the coding rate. */
    if (config.spreading_factor >= 6U && config.rx_bandwidth_hz > 0U &&
        config.coding_rate_denominator > 4U) {
        const double sf = config.spreading_factor;
        const double symbols_per_second =
            (double)config.rx_bandwidth_hz / ldexp(1.0, config.spreading_factor);
        _bitrate = (uint32_t)(sf * symbols_per_second * 4.0 /
                              (double)config.coding_rate_denominator);
    }
    solar_os_rnode_assembler_init(&assembler_);
}

bool ReticulumLoraInterface::start()
{
    const esp_err_t error =
        solar_os_radio_handle_set_state(&handle_, SOLAR_OS_RADIO_STATE_RX);
    _online = error == ESP_OK;
    return _online;
}

void ReticulumLoraInterface::stop()
{
    _online = false;
}

void ReticulumLoraInterface::back_to_receive()
{
    _online = solar_os_radio_handle_set_state(&handle_, SOLAR_OS_RADIO_STATE_RX) ==
              ESP_OK;
}

void ReticulumLoraInterface::loop()
{
    if (!_online) {
        return;
    }
    for (int budget = 0; budget < kReceiveBudget; budget++) {
        solar_os_radio_packet_t packet{};
        const esp_err_t error =
            solar_os_radio_handle_receive(&handle_, &packet, 0);
        if (error != ESP_OK) {
            return;
        }
        if (!packet.crc_ok || packet.len == 0U) {
            crc_errors_++;
            continue;
        }
        heard_ = true;
        if (packet.has_rssi) {
            last_rssi_ = packet.rssi_dbm;
        }
        if (packet.has_snr) {
            last_snr_ = packet.snr_db;
        }
        const size_t length = solar_os_rnode_assembler_feed(
            &assembler_, packet.data, packet.len, packet_, sizeof(packet_));
        if (length > 0U) {
            handle_incoming(RNS::Bytes(packet_, length));
        }
    }
}

uint32_t ReticulumLoraInterface::airtime_ms(size_t length) const
{
    if (config_.rx_bandwidth_hz == 0U || config_.spreading_factor < 6U) {
        return 1U;
    }
    const int sf = config_.spreading_factor;
    const double symbol_ms =
        ldexp(1000.0, sf) / (double)config_.rx_bandwidth_hz;
    const int low_data_rate = symbol_ms > 16.0 ? 1 : 0;
    const int crc = config_.crc_enabled ? 1 : 0;
    const int numerator = 8 * (int)length - 4 * sf + 28 + 16 * crc;
    const int denominator = 4 * (sf - 2 * low_data_rate);
    const int coding = std::max(1, (int)config_.coding_rate_denominator - 4);
    double payload_symbols = 8.0;
    if (numerator > 0 && denominator > 0) {
        payload_symbols += ceil((double)numerator / (double)denominator) *
                           (double)(coding + 4);
    }
    const double preamble = ((double)config_.preamble_len + 4.25) * symbol_ms;
    return (uint32_t)ceil(preamble + payload_symbols * symbol_ms);
}

bool ReticulumLoraInterface::send_outgoing(const RNS::Bytes &data)
{
    if (!_online) {
        return false;
    }
    solar_os_rnode_frame_t frames[2];
    const size_t count = solar_os_rnode_frame_pack(
        data.data(), data.size(), (uint8_t)(esp_random() & 0x0FU), frames);
    if (count == 0U) {
        send_errors_++;
        return false;
    }
    for (size_t index = 0U; index < count; index++) {
        solar_os_radio_packet_t packet{};
        packet.len = frames[index].len;
        memcpy(packet.data, frames[index].data, packet.len);
        const uint32_t timeout =
            std::max(kSendTimeoutFloorMs, airtime_ms(packet.len) * 2U + 500U);
        const esp_err_t error =
            solar_os_radio_handle_send(&handle_, &packet, timeout);
        if (error != ESP_OK) {
            send_errors_++;
            back_to_receive();
            return false;
        }
    }
    back_to_receive();
    InterfaceImpl::handle_outgoing(data);
    return true;
}

}  // namespace solar_os
