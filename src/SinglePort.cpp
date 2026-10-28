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
    c.event_user = this;
    c.event_pending_handler = [](void *user) {
        auto &self = *static_cast<SinglePort *>(user);
        if (self.config_.event_pending_handler)
            self.config_.event_pending_handler(self.config_.event_user);
    };
    c.event_handler = [](void *user, const iolink_master_event_t *event) {
        auto &self = *static_cast<SinglePort *>(user);
        self.pushEvent(*event);
        if (self.config_.event_handler)
            self.config_.event_handler(self.config_.event_user, event);
    };
    last_error_ = iolink_master_init(&port_, phy_, &c);
    initialized_ = true;
    running_ = last_error_ == 0;
    sample_time_ = 0;
    event_active_ = false;
    event_error_ = 0;
    return last_error_;
}
void SinglePort::stop() {
    if (pending_)
        finish(LinkLost);
    if (initialized_)
        iolink_master_set_port_mode(&port_, IOLINK_MASTER_PORT_MODE_DEACTIVATED);
    running_ = false;
    event_active_ = false;
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
uint32_t SinglePort::isduTimeout100us() const {
    iolink_master_device_info_t info{};
    if (deviceInfo(info) != 0)
        return 300000U;
    const uint32_t od = info.operate_mseq_code == 5 ? 2U : 1U;
    // One maximum-sized PDU + its peer header/error response, completion and
    // one full event readout. Allow two retries per message, device BUSY, and
    // 1 s of task/transport margin. Keep elapsed-time comparisons wrap-safe.
    const uint32_t cycles = (238U + od - 1U) / od + (6U + od - 1U) / od + 24U;
    uint32_t slot = cycleTime();
    if (slot < config_.response_timeout_100us)
        slot = config_.response_timeout_100us;
    if (slot < 20U)
        slot = 20U;
    uint64_t budget = uint64_t(cycles) * (uint64_t(slot) + 1U) * 3U +
                      (config_.isdu_timeout_100us ? config_.isdu_timeout_100us : 50000U) + 10000U;
    if (budget < 300000U)
        budget = 300000U;
    return budget > 0x7fffffffU ? 0x7fffffffU : uint32_t(budget);
}
bool SinglePort::popEvent(Event &event) {
    if (!event_count_)
        return false;
    event = events_[event_head_];
    event_head_ = (event_head_ + 1U) % EventCapacity;
    --event_count_;
    return true;
}
void SinglePort::pollEvents() {
    if (state() != IOLINK_MASTER_STATE_OPERATE) {
        if (event_active_)
            event_error_ = LinkLost;
        event_active_ = false;
        return;
    }
    if (!event_active_ && !diagnostics().event_pending)
        return;
    iolink_master_event_t batch[6]{};
    uint8_t count = 0;
    const int rc = iolink_master_read_and_ack_events(&port_, batch, 6, &count);
    event_active_ = rc == IOLINK_MASTER_STATUS_PENDING;
    if (event_active_)
        return;
    event_error_ = rc;
}
void SinglePort::pushEvent(const iolink_master_event_t &value) {
    if (event_count_ == EventCapacity) {
        event_head_ = (event_head_ + 1U) % EventCapacity;
        --event_count_;
        ++dropped_events_;
    }
    auto &event = events_[(event_head_ + event_count_) % EventCapacity];
    event.sequence = ++event_sequence_;
    event.time100us = now_;
    event.value = value;
    ++event_count_;
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
    request_timeout_ = isduTimeout100us();
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
    pollEvents();
    iolink_master_timing_t timing{};
    iolink_master_get_timing(&port_, &timing);
    auto event = timing.awaiting_response && int32_t(now - timing.response_deadline_100us) >= 0
                     ? IOLINK_MASTER_TICK_RESPONSE_TIMEOUT
                     : IOLINK_MASTER_TICK_CYCLE_DUE;
    const int rc = iolink_master_tick_at(&port_, event, now);
    pollEvents();
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
    if (uint32_t(now - request_time_) >= request_timeout_) {
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
