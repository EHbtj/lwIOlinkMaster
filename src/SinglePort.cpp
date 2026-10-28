#include "SinglePort.hpp"
#include <cstring>
namespace lwIOLink {
SinglePort::SinglePort(const iolink_phy_api_t *p, const iolink_master_config_t &c)
    : phy_(p), config_(c) {}
int SinglePort::start() {
    if (pending_)
        return Busy;
    if (running_)
        return 0; // idempotent: never reset a live link implicitly
    auto c = config_;
    c.port_mode = IOLINK_MASTER_PORT_MODE_IOLINK;
    last_error_ = iolink_master_init(&port_, phy_, &c);
    initialized_ = true;
    running_ = last_error_ == 0;
    sample_time_ = 0;
    return last_error_;
}
void SinglePort::stop() {
    if (pending_)
        finish(LinkLost);
    if (initialized_)
        iolink_master_set_port_mode(&port_, IOLINK_MASTER_PORT_MODE_DEACTIVATED);
    running_ = false;
    sample_time_ = 0;
}
iolink_master_state_t SinglePort::state() const {
    return initialized_ ? iolink_master_get_state(&port_) : IOLINK_MASTER_STATE_INACTIVE;
}
int SinglePort::deviceInfo(iolink_master_device_info_t &info) const {
    info = {};
    return initialized_ ? iolink_master_get_device_info(&port_, &info) : NotReady;
}
int SinglePort::processData(uint8_t *data, uint8_t capacity, uint8_t &size) const {
    if (state() != IOLINK_MASTER_STATE_OPERATE) {
        size = 0;
        return NotReady;
    }
    return iolink_master_get_pd_in(&port_, data, capacity, &size);
}
iolink_master_diagnostics_t SinglePort::diagnostics() const {
    iolink_master_diagnostics_t d{};
    if (initialized_)
        iolink_master_get_diagnostics(&port_, &d);
    return d;
}
uint16_t SinglePort::cycleTime() const {
    iolink_master_timing_t t{};
    if (initialized_)
        iolink_master_get_timing(&port_, &t);
    return t.min_cycle_time_100us;
}
void SinglePort::finish(int code) {
    pending_ = false;
    result_.ready = true;
    result_.code = code;
    result_.device_error = diagnostics().last_isdu_error;
    if (code)
        result_.size = 0;
}
int SinglePort::requestRead(uint16_t i, uint8_t sub) {
    if (pending_)
        return Busy;
    if (state() != IOLINK_MASTER_STATE_OPERATE)
        return NotReady;
    iolink_master_device_info_t info{};
    if (deviceInfo(info) != 0 || !info.isdu_supported)
        return Unsupported;
    index_ = i;
    subindex_ = sub;
    writing_ = false;
    pending_ = true;
    request_time_ = now_;
    result_ = {};
    return 0;
}
int SinglePort::requestWrite(uint16_t i, uint8_t sub, const uint8_t *data, size_t size) {
    if (!data || size == 0 || size > MaxIsdu)
        return IOLINK_MASTER_ERR_INVALID_ARG;
    int rc = requestRead(i, sub);
    if (rc)
        return rc;
    writing_ = true;
    write_size_ = static_cast<uint8_t>(size);
    std::memcpy(write_data_, data, size);
    return 0;
}
void SinglePort::poll(uint32_t now) {
    now_ = now;
    if (!running_)
        return;
    iolink_master_timing_t timing{};
    iolink_master_get_timing(&port_, &timing);
    auto event = timing.awaiting_response && int32_t(now - timing.response_deadline_100us) >= 0
                     ? IOLINK_MASTER_TICK_RESPONSE_TIMEOUT
                     : IOLINK_MASTER_TICK_CYCLE_DUE;
    const int rc = iolink_master_tick_at(&port_, event, now);
    if (rc < 0)
        last_error_ = rc;
    if (state() == IOLINK_MASTER_STATE_ERROR) {
        last_error_ = diagnostics().last_service_result ? diagnostics().last_service_result : rc;
        if (pending_)
            finish(LinkLost);
        running_ = false;
        return; // explicit start can retry hardware/config errors
    }
    if (rc > 0 && state() == IOLINK_MASTER_STATE_OPERATE)
        sample_time_ = now;
    if (!pending_)
        return;
    if (state() != IOLINK_MASTER_STATE_OPERATE) {
        finish(LinkLost);
        return;
    }
    if (uint32_t(now - request_time_) >= 300000U) {
        finish(Timeout);
        stop();
        return;
    }
    uint8_t length = MaxIsdu;
    int service =
        writing_ ? iolink_master_write_isdu(&port_, index_, subindex_, write_data_, write_size_)
                 : iolink_master_read_isdu(&port_, index_, subindex_, result_.data, &length);
    if (service != IOLINK_MASTER_STATUS_PENDING) {
        result_.size = writing_ ? 0 : length;
        finish(service);
    }
}
} // namespace lwIOLink
