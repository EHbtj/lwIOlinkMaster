#pragma once
#include "../phy_driver.hpp"
#include <Arduino.h>
#include <driver/uart.h>
extern "C" {
#include "iolinki_master/master.h"
}
namespace lwIOLink::driver::phy {
// Single task / single physical port. Owns UART1, never share with Serial1.
class TIOL112 : public Phy {
  public:
    struct tiol_cfg : public Phy::Hw_cfg_t {
        int tx_pin = -1, rx_pin = -1, tx_en = -1, fault_pin = -1;
        uint8_t uart_num = 1;
    };
    explicit TIOL112(const tiol_cfg *cfg) : Phy(1), cfg_(*cfg) {}
    bool begin() override;
    bool wakeup(uint8_t port) override;
    void setCOM_Mode(uint8_t port, COM_e mode) override;
    bool PLTransferReq(uint8_t port, const uint8_t *data, uint8_t len, uint8_t rxlen) override;
    void RecieveTask() override;
    const iolink_phy_api_t *api();
    void configureHooks(iolink_master_config_t &);
    bool fault() const;
    void waitEvent(TickType_t ticks);
    uint32_t baudrate() const { return baud_; }
    static uint32_t responseWindowUs(uint32_t baud, size_t tx, size_t rx) {
        return uint32_t(((tx * 12ULL + rx * 14ULL + 10ULL) * 1000000ULL + baud - 1) / baud) + 200;
    }

  private:
    static TIOL112 *active_;
    tiol_cfg cfg_;
    iolink_phy_api_t api_{};
    QueueHandle_t events_ = nullptr;
    bool initialized_ = false, rx_error_ = false, active_mode_ = false;
    uint32_t baud_ = 230400, legacy_start_ = 0, legacy_timeout_ = 0;
    uint8_t legacy_expected_ = 0, legacy_count_ = 0, legacy_buffer_[RX_BUFF_SIZE]{};
    uart_port_t uart() const { return static_cast<uart_port_t>(cfg_.uart_num); }
    int setBaud(iolink_baudrate_t);
    int setMode(iolink_phy_mode_t);
    int flushRx();
    int send(const uint8_t *, size_t);
    int receive(uint8_t *);
    void consumeEvent(const uart_event_t &);
};
} // namespace lwIOLink::driver::phy
