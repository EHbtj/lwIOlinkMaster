#pragma once
#include "SinglePort.hpp"
namespace lwIOLink {
// Bounded NDJSON endpoint. feed() returns false under output backpressure.
// poll() never blocks; IO-Link is polled independently even when hasReply().
class UsbProtocol {
  public:
    static constexpr size_t MaxLine = 768, MaxReply = 1024;
    explicit UsbProtocol(SinglePort &port) : port_(port) {}
    bool feed(char c);
    void poll();
    bool hasReply() const { return reply_[0] != 0; }
    const char *reply() const { return reply_; }
    void consumeReply() { reply_[0] = 0; }
    void disconnect(); // clear partial transport only; no cancellation/replay of ISDU
  private:
    SinglePort &port_;
    char line_[MaxLine + 1]{}, reply_[MaxReply]{};
    size_t used_ = 0;
    bool overflow_ = false, waiting_ = false;
    uint32_t waiting_id_ = 0;
    void dispatch();
    void error(uint32_t id, bool valid_id, const char *message, int code = 0, uint16_t device = 0);
};
} // namespace lwIOLink
