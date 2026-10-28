#include "tiol112.hpp"
#include <driver/gpio.h>
#include <esp_rom_sys.h>
namespace lwIOLink::driver::phy {
TIOL112 *TIOL112::active_ = nullptr;
bool TIOL112::begin() {
    if (initialized_)
        return true;
    if ((active_ && active_ != this) || cfg_.uart_num != 1 ||
        !GPIO_IS_VALID_OUTPUT_GPIO(cfg_.tx_pin) || !GPIO_IS_VALID_GPIO(cfg_.rx_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(cfg_.tx_en) || !GPIO_IS_VALID_GPIO(cfg_.fault_pin))
        return false;
    const int pins[] = {cfg_.tx_pin, cfg_.rx_pin, cfg_.tx_en, cfg_.fault_pin};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = i + 1; j < 4; ++j)
            if (pins[i] == pins[j])
                return false;
    pinMode(cfg_.tx_en, OUTPUT);
    digitalWrite(cfg_.tx_en, LOW);
    pinMode(cfg_.fault_pin, INPUT_PULLUP);
    uart_config_t s{};
    s.baud_rate = baud_;
    s.data_bits = UART_DATA_8_BITS;
    s.parity = UART_PARITY_EVEN;
    s.stop_bits = UART_STOP_BITS_1;
    s.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    s.source_clk = UART_SCLK_DEFAULT;
    if (uart_is_driver_installed(uart()) || uart_param_config(uart(), &s) != ESP_OK ||
        uart_driver_install(uart(), 512, 0, 32, &events_, 0) != ESP_OK)
        return false;
    initialized_ = true;
    active_ = this;
    if (uart_set_pin(uart(), cfg_.tx_pin, cfg_.rx_pin, cfg_.tx_en, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_set_mode(uart(), UART_MODE_RS485_HALF_DUPLEX) != ESP_OK ||
        uart_set_rx_full_threshold(uart(), 1) != ESP_OK ||
        uart_set_rx_timeout(uart(), 1) != ESP_OK) {
        uart_driver_delete(uart());
        initialized_ = false;
        active_ = nullptr;
        digitalWrite(cfg_.tx_en, LOW);
        return false;
    }
    return true;
}
int TIOL112::setMode(iolink_phy_mode_t m) {
    if (!initialized_)
        return -1;
    active_mode_ = m == IOLINK_PHY_MODE_SDCI;
    legacy_expected_ = 0;
    return flushRx();
}
int TIOL112::setBaud(iolink_baudrate_t m) {
    static const uint32_t rates[] = {4800, 38400, 230400};
    if (!initialized_ || unsigned(m) > 2)
        return -1;
    baud_ = rates[m];
    return uart_set_baudrate(uart(), baud_) == ESP_OK ? flushRx() : -1;
}
bool TIOL112::wakeup(uint8_t port) {
    if (port != 1 || !initialized_ || fault() ||
        uart_wait_tx_done(uart(), pdMS_TO_TICKS(100)) != ESP_OK)
        return false;
    // RX is inverted C/Q. Drive TX=!RX to oppose the current line state.
    gpio_reset_pin(static_cast<gpio_num_t>(cfg_.tx_en));
    gpio_reset_pin(static_cast<gpio_num_t>(cfg_.tx_pin));
    gpio_set_direction(static_cast<gpio_num_t>(cfg_.tx_en), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(cfg_.tx_pin), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.tx_en), 0);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.tx_pin),
                   !gpio_get_level(static_cast<gpio_num_t>(cfg_.rx_pin)));
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.tx_en), 1);
    esp_rom_delay_us(80);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.tx_en), 0);
    portEXIT_CRITICAL(&mux);
    auto result = uart_set_pin(uart(), cfg_.tx_pin, cfg_.rx_pin, cfg_.tx_en, UART_PIN_NO_CHANGE);
    flushRx();
    return result == ESP_OK;
}
bool TIOL112::fault() const { return initialized_ && digitalRead(cfg_.fault_pin) == LOW; }
int TIOL112::flushRx() {
    if (!initialized_)
        return -1;
    uart_flush_input(uart());
    xQueueReset(events_);
    rx_error_ = false;
    return 0;
}
int TIOL112::send(const uint8_t *data, size_t len) {
    if (!initialized_ || !active_mode_ || !data || !len || len > RX_BUFF_SIZE || fault())
        return -1;
    // RS485 half duplex suppresses TX echo and releases EN/RTS in TX_DONE ISR.
    int sent = uart_write_bytes(uart(), data, len);
    if (sent != int(len) || uart_wait_tx_done(uart(), pdMS_TO_TICKS(250)) != ESP_OK) {
        active_mode_ = false;
        return -1;
    }
    return sent;
}
void TIOL112::consumeEvent(const uart_event_t &e) {
    if (e.type == UART_PARITY_ERR || e.type == UART_FRAME_ERR || e.type == UART_FIFO_OVF ||
        e.type == UART_BUFFER_FULL || e.type == UART_BREAK)
        rx_error_ = true;
}
void TIOL112::waitEvent(TickType_t ticks) {
    uart_event_t e;
    if (events_ && xQueueReceive(events_, &e, ticks) == pdTRUE)
        consumeEvent(e);
}
int TIOL112::receive(uint8_t *byte) {
    uart_event_t e;
    while (xQueueReceive(events_, &e, 0) == pdTRUE)
        consumeEvent(e);
    if (rx_error_ || fault()) {
        flushRx();
        return -1;
    }
    return uart_read_bytes(uart(), byte, 1, 0);
}
const iolink_phy_api_t *TIOL112::api() {
    api_.user = this;
    api_.init = [](void *p) { return static_cast<TIOL112 *>(p)->begin() ? 0 : -1; };
    api_.set_mode = [](void *p, iolink_phy_mode_t m) { static_cast<TIOL112 *>(p)->setMode(m); };
    api_.set_baudrate = [](void *p, iolink_baudrate_t b) { static_cast<TIOL112 *>(p)->setBaud(b); };
    api_.send = [](void *p, const uint8_t *d, size_t n) {
        return static_cast<TIOL112 *>(p)->send(d, n);
    };
    api_.recv_byte = [](void *p, uint8_t *b) { return static_cast<TIOL112 *>(p)->receive(b); };
    return &api_;
}
void TIOL112::configureHooks(iolink_master_config_t &c) {
    c.set_mode_checked = [](iolink_phy_mode_t m) { return active_ ? active_->setMode(m) : -1; };
    c.set_baudrate_checked = [](iolink_baudrate_t b) { return active_ ? active_->setBaud(b) : -1; };
    c.flush_rx = []() { return active_ ? active_->flushRx() : -1; };
    c.prepare_tx = []() { return active_ && !active_->fault() ? 0 : -1; };
    c.prepare_rx = []() { return active_ ? 0 : -1; };
    c.wake_up = []() { return active_ && active_->wakeup(1) ? 0 : -1; };
}
void TIOL112::setCOM_Mode(uint8_t p, COM_e m) {
    if (p != 1 || !initialized_)
        return;
    if (m == COM_e::SIO) {
        setMode(IOLINK_PHY_MODE_INACTIVE);
        return;
    }
    setMode(IOLINK_PHY_MODE_SDCI);
    setBaud(m == COM_e::COM1   ? IOLINK_BAUDRATE_COM1
            : m == COM_e::COM2 ? IOLINK_BAUDRATE_COM2
                               : IOLINK_BAUDRATE_COM3);
}
bool TIOL112::PLTransferReq(uint8_t p, const uint8_t *d, uint8_t n, uint8_t rx) {
    if (p != 1 || !rx || rx > RX_BUFF_SIZE || legacy_expected_)
        return false;
    flushRx();
    if (send(d, n) != n)
        return false;
    legacy_count_ = 0;
    legacy_expected_ = rx;
    legacy_start_ = micros();
    legacy_timeout_ = responseWindowUs(baud_, 0, rx);
    return true;
}
void TIOL112::RecieveTask() {
    if (!legacy_expected_)
        return;
    int n = 0;
    while (legacy_count_ < legacy_expected_ && (n = receive(&legacy_buffer_[legacy_count_])) > 0)
        ++legacy_count_;
    if (legacy_count_ == legacy_expected_) {
        auto count = legacy_expected_;
        legacy_expected_ = 0;
        if (m_transferInd_cb)
            m_transferInd_cb(1, legacy_buffer_, count);
    } else if (n < 0 || uint32_t(micros() - legacy_start_) >= legacy_timeout_) {
        legacy_expected_ = 0;
        if (m_fault_cb)
            m_fault_cb(1, fault_e::MSeqTimeout);
    }
}
} // namespace lwIOLink::driver::phy
