#include "SinglePort.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "fake_iolink_device.h"
}

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

// Replies are immediate, making the master responsible for all request pacing.
// The wrapper also refuses identity/OPERATE access until MasterIdent's ACK is read.
struct Transport {
    struct Frame {
        uint32_t time;
        iolink_baudrate_t baud;
        std::vector<uint8_t> bytes;
    };
    static Transport *active;
    iolink_phy_api_t api{};
    uint32_t now = 0;
    iolink_baudrate_t baud = IOLINK_BAUDRATE_COM3, target;
    std::vector<Frame> frames;
    std::vector<uint8_t> reply, retry_request, retry_reply;
    size_t pos = 0;
    bool identified = false, ack_valid = false, violation = false;
    bool scan_errors = false, retry_pending = false;
    int pending_error = 0, fault_mc = -1, fault_count = 0;
    int fault_code = IOLINK_PHY_RX_ERROR;
    unsigned bytes_before_error = 0, returned = 0;
    unsigned ident_attempts = 0, ident_drops = 0, ident_corruptions = 0;
    unsigned injected = 0, repeated = 0;

    explicit Transport(iolink_baudrate_t rate = IOLINK_BAUDRATE_COM3) : target(rate) {
        active = this;
        fake_iolink_device_reset(0xa5, 1, 1);
        fake_iolink_device_set_direct_parameter_page1(20, 1, 8, 0, 0x1234, 0x56789a);
        api = *fake_iolink_device_phy();
        api.user = this;
        api.send = [](void *p, const uint8_t *d, size_t n) {
            return static_cast<Transport *>(p)->send(d, n);
        };
        api.recv_byte = [](void *p, uint8_t *b) { return static_cast<Transport *>(p)->receive(b); };
        api.set_baudrate = [](void *p, iolink_baudrate_t b) {
            static_cast<Transport *>(p)->baud = b;
        };
    }
    void flush() {
        reply.clear();
        pos = 0;
        pending_error = 0;
        ack_valid = false;
        fake_iolink_device_flush_rx();
    }
    iolink_master_config_t config() {
        iolink_master_config_t c{};
        c.auto_configure = true;
        c.auto_baudrate = true;
        c.baudrate = IOLINK_BAUDRATE_COM3;
        c.m_seq_type = IOLINK_MASTER_M_SEQ_TYPE_0;
        c.flush_rx = []() {
            active->flush();
            return 0;
        };
        return c;
    }
    int send(const uint8_t *data, size_t n) {
        frames.push_back({now, baud, {data, data + n}});
        flush();
        returned = 0;
        if (n == 1 && data[0] == 0x55) {
            identified = false;
            retry_pending = false;
            return fake_iolink_device_phy()->send(nullptr, data, n);
        }
        if (baud != target) {
            if (scan_errors) {
                pending_error = IOLINK_PHY_RX_ERROR;
                ++injected;
            }
            return int(n);
        }
        const bool ident = n == 3 && data[0] == 0x20 && data[2] == 0x95;
        const bool identity = n == 2 && (data[0] & 0xe0) == 0xa0 && (data[0] & 31) >= 7;
        const bool operate = n == 3 && data[0] == 0x20 && data[2] == 0x99;
        if ((identity || operate || data[0] == 0x21) && !identified) {
            violation = true;
            return int(n);
        }
        if (retry_pending) {
            CHECK(retry_request == frames.back().bytes);
            reply = retry_reply;
            retry_pending = false;
            ++repeated;
        } else {
            CHECK(fake_iolink_device_phy()->send(nullptr, data, n) == int(n));
            uint8_t b;
            while (fake_iolink_device_phy()->recv_byte(nullptr, &b) > 0)
                reply.push_back(b);
        }
        if (ident) {
            ++ident_attempts;
            if (ident_drops) {
                --ident_drops;
                reply.clear();
            } else if (ident_corruptions) {
                --ident_corruptions;
                CHECK(reply.size() == 1);
                reply[0] ^= 1;
            } else
                ack_valid = true;
        }
        if (fault_count && data[0] == fault_mc) {
            --fault_count;
            ++injected;
            retry_request.assign(data, data + n);
            retry_reply = reply;
            retry_pending = true;
            pending_error = fault_code;
            ack_valid = false;
        }
        return int(n);
    }
    int receive(uint8_t *b) {
        if (pending_error && returned >= bytes_before_error) {
            const int error = pending_error;
            pending_error = 0;
            return error;
        }
        if (pos == reply.size())
            return 0;
        *b = reply[pos++];
        ++returned;
        if (ack_valid && pos == reply.size())
            identified = true;
        return 1;
    }
    void poll(lwIOLink::SinglePort &p) { p.poll(now++); }
    void advance(lwIOLink::SinglePort &p, unsigned ticks) {
        while (ticks--)
            poll(p);
    }
    void untilOperate(lwIOLink::SinglePort &p) {
        for (unsigned n = 0; n < 50000 && p.state() != IOLINK_MASTER_STATE_OPERATE; ++n) {
            poll(p);
            CHECK(p.state() != IOLINK_MASTER_STATE_ERROR);
        }
        CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
        CHECK(identified && !violation);
    }
    void checkStartupSpacing() const {
        static const uint32_t rates[] = {4800, 38400, 230400};
        bool have_prior = false;
        uint32_t prior = 0;
        for (const auto &f : frames) {
            if (f.bytes.size() == 1 || f.baud != target) {
                have_prior = false;
                continue;
            }
            // Independent conversion of 100 bit periods to 100us ticks.
            const uint32_t minimum = (1000000U + rates[target] - 1) / rates[target];
            if (have_prior)
                CHECK(uint32_t(f.time - prior) >= minimum + 1);
            prior = f.time;
            have_prior = true;
            if (f.bytes.size() == 3 && f.bytes[0] == 0x20 && f.bytes[2] == 0x99)
                have_prior = false;
        }
    }
};
Transport *Transport::active = nullptr;

static void timing_and_ident() {
    for (auto baud : {IOLINK_BAUDRATE_COM1, IOLINK_BAUDRATE_COM2, IOLINK_BAUDRATE_COM3}) {
        for (uint32_t start : {0U, 0xffffff80U}) {
            Transport t(baud);
            t.now = start;
            lwIOLink::SinglePort p(&t.api, t.config());
            CHECK(p.start() == 0);
            t.untilOperate(p);
            CHECK(t.ident_attempts == 1);
            t.checkStartupSpacing();

            // Event-driven C callers must see the same parameter-read gate.
            Transport scheduled(baud);
            scheduled.now = start;
            auto config = scheduled.config();
            iolink_master_port_t core{};
            CHECK(iolink_master_init(&core, &scheduled.api, &config) == 0);
            bool checked_gate = false;
            for (unsigned n = 0; n < 5000 && !checked_gate; ++n, ++scheduled.now) {
                iolink_master_timing_t timing{};
                CHECK(iolink_master_get_timing(&core, &timing) == 0);
                const auto event =
                    timing.awaiting_response &&
                            int32_t(scheduled.now - timing.response_deadline_100us) >= 0
                        ? IOLINK_MASTER_TICK_RESPONSE_TIMEOUT
                        : IOLINK_MASTER_TICK_CYCLE_DUE;
                iolink_master_tick_at(&core, event, scheduled.now);
                CHECK(iolink_master_get_timing(&core, &timing) == 0);
                if (iolink_master_get_state(&core) == IOLINK_MASTER_STATE_PREOPERATE &&
                    !timing.awaiting_response) {
                    uint32_t next = scheduled.now;
                    CHECK(iolink_master_get_next_tick_time(&core, scheduled.now, &next) == 0);
                    CHECK(int32_t(next - scheduled.now) > 0);
                    checked_gate = true;
                }
            }
            CHECK(checked_gate);
        }
    }
}
static void ident_retries() {
    for (auto baud : {IOLINK_BAUDRATE_COM1, IOLINK_BAUDRATE_COM2, IOLINK_BAUDRATE_COM3}) {
        Transport t(baud);
        t.ident_drops = 1;
        t.ident_corruptions = 1;
        lwIOLink::SinglePort p(&t.api, t.config());
        CHECK(p.start() == 0);
        t.untilOperate(p);
        CHECK(t.ident_attempts == 3 && p.diagnostics().checksum_errors == 1);
        t.checkStartupSpacing();
    }
}
static void scan_errors() {
    for (auto baud : {IOLINK_BAUDRATE_COM1, IOLINK_BAUDRATE_COM2}) {
        Transport t(baud);
        t.scan_errors = true;
        lwIOLink::SinglePort p(&t.api, t.config());
        CHECK(p.start() == 0);
        t.untilOperate(p);
        CHECK(t.injected == (baud == IOLINK_BAUDRATE_COM1 ? 2U : 1U));
        CHECK(fake_iolink_device_wakeup_count() == 1);
    }
}
static void transient_errors() {
    for (int mc : {0xa3, 0x20, 0xf1}) {
        Transport t;
        t.fault_mc = mc;
        t.fault_count = 1;
        lwIOLink::SinglePort p(&t.api, t.config());
        CHECK(p.start() == 0);
        t.untilOperate(p);
        if (mc == 0xf1) {
            for (unsigned n = 0; n < 1000 && !t.injected; ++n)
                t.poll(p);
            CHECK(t.injected == 1);
            t.poll(p); // Consume the injected UART error before any retransmit.
            uint8_t bytes[32], size = 0;
            CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
            CHECK(p.processData(bytes, sizeof(bytes), size) == IOLINK_MASTER_STATUS_PENDING);
        }
        t.advance(p, 1000);
        CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE && t.injected == 1 && t.repeated == 1);
        uint8_t bytes[32], size = 0;
        CHECK(p.processData(bytes, sizeof(bytes), size) == 0 && size == 1 && bytes[0] == 0xa5);
        t.checkStartupSpacing();
    }
}
static void repeated_errors() {
    Transport t;
    lwIOLink::SinglePort p(&t.api, t.config());
    CHECK(p.start() == 0);
    t.untilOperate(p);
    t.advance(p, 100);
    t.fault_mc = 0xf1;
    t.fault_count = 3;
    for (unsigned n = 0; n < 10000 && p.state() == IOLINK_MASTER_STATE_OPERATE; ++n)
        t.poll(p);
    CHECK(p.state() == IOLINK_MASTER_STATE_STARTUP);
    uint8_t bytes[32], size = 0;
    CHECK(p.processData(bytes, sizeof(bytes), size) == lwIOLink::SinglePort::NotReady);
    iolink_master_device_info_t info{};
    CHECK(p.deviceInfo(info) != 0);
    t.untilOperate(p);
    t.advance(p, 100);
    CHECK(t.injected == 3 && t.repeated == 2);
    CHECK(t.ident_attempts == 2 && fake_iolink_device_wakeup_count() == 2);
    CHECK(p.processData(bytes, sizeof(bytes), size) == 0);
}
static void partial_isdu_errors() {
    for (int mc : {0x61, 0xe1}) {
        Transport t;
        lwIOLink::SinglePort p(&t.api, t.config());
        CHECK(p.start() == 0);
        t.untilOperate(p);
        t.advance(p, 100);
        const uint8_t value[] = {1, 2, 3, 4, 5};
        fake_iolink_device_set_isdu_object(0x1234, 2, value, sizeof(value));
        t.fault_mc = mc;
        t.fault_count = 1;
        t.bytes_before_error = 1;
        CHECK(p.requestRead(0x1234, 2) == 0);
        for (unsigned n = 0; n < 10000 && !p.result().ready; ++n)
            t.poll(p);
        CHECK(p.result().ready && p.result().code == 0 && p.result().size == sizeof(value));
        CHECK(!std::memcmp(p.result().data, value, sizeof(value)));
        CHECK(t.injected == 1 && t.repeated == 1 && p.state() == IOLINK_MASTER_STATE_OPERATE);
    }
}
static void fatal_error() {
    Transport t;
    lwIOLink::SinglePort p(&t.api, t.config());
    CHECK(p.start() == 0);
    t.untilOperate(p);
    t.advance(p, 100);
    t.fault_mc = 0xf1;
    t.fault_count = 1;
    t.fault_code = IOLINK_PHY_RX_FAULT;
    for (unsigned n = 0; n < 1000 && p.state() != IOLINK_MASTER_STATE_ERROR; ++n)
        t.poll(p);
    CHECK(p.state() == IOLINK_MASTER_STATE_ERROR);
    const auto sent = t.frames.size();
    t.advance(p, 10000);
    CHECK(p.state() == IOLINK_MASTER_STATE_ERROR && t.frames.size() == sent);
    CHECK(p.start() == 0);
    t.untilOperate(p);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!std::strcmp(argv[1], "timing"))
        timing_and_ident();
    else if (!std::strcmp(argv[1], "ident_retries"))
        ident_retries();
    else if (!std::strcmp(argv[1], "scan_errors"))
        scan_errors();
    else if (!std::strcmp(argv[1], "transient_errors"))
        transient_errors();
    else if (!std::strcmp(argv[1], "repeated_errors"))
        repeated_errors();
    else if (!std::strcmp(argv[1], "partial_isdu_errors"))
        partial_isdu_errors();
    else if (!std::strcmp(argv[1], "fatal_error"))
        fatal_error();
    else
        CHECK(false);
    std::printf("PASS: %s\n", argv[1]);
}
