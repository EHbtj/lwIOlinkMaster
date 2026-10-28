#pragma once
#include <cstdint>
#include <cstddef>
extern "C" {
#include "iolinki_master/master.h"
}
namespace lwIOLink {
// Portable, single-owner API. Call poll at least every 100us or on UART events.
class SinglePort {
  public:
    static constexpr size_t MaxIsdu = 232;
    static constexpr uint8_t EventCapacity = 16;
    struct Event {
        uint32_t sequence = 0;
        uint32_t time100us = 0;
        iolink_master_event_t value{};
    };
    enum Error {
        Busy = -100,
        NotReady = -101,
        Unsupported = -102,
        LinkLost = -103,
        Timeout = -104
    };
    struct Result {
        bool ready = false;
        int code = 0;
        uint16_t device_error = 0;
        uint8_t size = 0;
        uint8_t data[MaxIsdu]{};
    };
    SinglePort(const iolink_phy_api_t *phy, const iolink_master_config_t &config);
    int start();
    void stop();
    void poll(uint32_t now100us);
    int requestRead(uint16_t index, uint8_t subindex);
    int requestWrite(uint16_t index, uint8_t subindex, const uint8_t *data, size_t size);
    bool busy() const { return pending_; }
    const Result &result() const { return result_; }
    iolink_master_state_t state() const;
    int deviceInfo(iolink_master_device_info_t &info) const;
    int processData(uint8_t *data, uint8_t capacity, uint8_t &size) const;
    iolink_master_diagnostics_t diagnostics() const;
    uint32_t sampleTime() const { return sample_time_; }
    uint16_t cycleTime() const;
    int lastError() const { return last_error_; }
    // Events are collected/acknowledged automatically, independently of USB.
    bool popEvent(Event &event);
    uint8_t eventCount() const { return event_count_; }
    uint32_t droppedEvents() const { return dropped_events_; }
    int lastEventError() const { return event_error_; }
    // Conservative overall ISDU budget for the current link (minimum 30 s).
    uint32_t isduTimeout100us() const;

  private:
    const iolink_phy_api_t *phy_;
    iolink_master_config_t config_;
    iolink_master_port_t port_{};
    bool initialized_ = false, running_ = false, pending_ = false, writing_ = false;
    uint16_t index_ = 0;
    uint8_t subindex_ = 0, write_size_ = 0;
    uint8_t write_data_[MaxIsdu]{};
    uint32_t now_ = 0, request_time_ = 0, sample_time_ = 0;
    uint32_t request_timeout_ = 300000;
    int last_error_ = 0;
    Result result_;
    Event events_[EventCapacity]{};
    uint8_t event_head_ = 0, event_count_ = 0;
    uint32_t event_sequence_ = 0, dropped_events_ = 0;
    bool event_active_ = false;
    int event_error_ = 0;
    void finish(int code);
    void pollEvents();
    void pushEvent(const iolink_master_event_t &value);
};
} // namespace lwIOLink
