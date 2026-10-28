#include "SinglePort.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "fake_iolink_device.h"
#include "iolinki/crc.h"
}

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    std::exit(1); } } while (0)

// Require an explicit IDLE/ABORT between services; the bundled fake accepts
// a new START while its previous response is still active.
struct Transport {
    static Transport *active;
    iolink_phy_api_t api{};
    uint32_t now = 0, busy_start = 0, busy_ticks = 0;
    bool busy_started = false, service_open = false, violation = false;
    bool drop_segments = false;
    int completion_fault = 0; // 1: lost ACK, 2: corrupt ACK
    unsigned completion_retries = 0;
    uint8_t od_len, pd_len;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> reply, retry_frame;
    std::vector<uint8_t> completion_retry_frame;
    size_t pos = 0;
    unsigned retries = 0;

    explicit Transport(uint8_t od = 1, uint8_t cycle = 0x80, uint8_t pd = 1)
        : od_len(od), pd_len(pd) {
        active = this;
        fake_iolink_device_reset(0xa5, pd, od);
        fake_iolink_device_set_direct_parameter_page1(cycle, od == 2 ? 11 : 1,
                                                    pd ? 8 : 0, 0, 0x1234, 0x56789a);
        api = *fake_iolink_device_phy();
        api.user = this;
        api.send = [](void *p, const uint8_t *d, size_t n) {
            return static_cast<Transport *>(p)->send(d, n);
        };
        api.recv_byte = [](void *p, uint8_t *b) {
            auto &t = *static_cast<Transport *>(p);
            if (t.pos == t.reply.size()) return 0;
            *b = t.reply[t.pos++];
            return 1;
        };
    }
    iolink_master_config_t config() {
        iolink_master_config_t c{};
        c.auto_configure = true;
        c.baudrate = IOLINK_BAUDRATE_COM3;
        c.m_seq_type = IOLINK_MASTER_M_SEQ_TYPE_0;
        c.flush_rx = []() {
            active->reply.clear();
            active->pos = 0;
            fake_iolink_device_flush_rx();
            return 0;
        };
        return c;
    }
    int send(const uint8_t *d, size_t n) {
        frames.emplace_back(d, d + n);
        reply.clear(); pos = 0;
        if (n == 1 && d[0] == 0x55) {
            service_open = false;
            return fake_iolink_device_phy()->send(nullptr, d, n);
        }
        const bool isdu = (d[0] & 0x60) == 0x60;
        if (!completion_retry_frame.empty()) {
            CHECK(frames.back() == completion_retry_frame);
            completion_retry_frame.clear();
            ++completion_retries;
        }
        const bool segment = isdu && (d[0] & 0x80) && (d[0] & 0x1f) < 0x10;
        if (drop_segments && segment) {
            if (retry_frame.empty()) {
                retry_frame = frames.back();
                fake_iolink_device_phy()->send(nullptr, d, n);
                fake_iolink_device_flush_rx();
            } else {
                CHECK(retry_frame == frames.back());
                ++retries;
            }
            return int(n);
        }
        if (isdu && d[0] == 0x70) {
            if (service_open) { violation = true; return int(n); }
            service_open = true;
        }
        if (isdu && (d[0] == 0xf1 || d[0] == 0xff)) service_open = false;
        if (isdu && d[0] == 0xf0 && busy_ticks) {
            if (!busy_started) { busy_start = now; busy_started = true; }
            if (uint32_t(now - busy_start) < busy_ticks) {
                // BUSY OD followed by valid PD and an independent link checksum.
                reply.assign(od_len, 0);
                reply[0] = 1;
                if (pd_len) reply.push_back(0xa5);
                reply.push_back(0);
                reply.back() = iolink_checksum6(reply.data(), reply.size());
                return int(n);
            }
        }
        const int rc = fake_iolink_device_phy()->send(nullptr, d, n);
        uint8_t b;
        while (fake_iolink_device_phy()->recv_byte(nullptr, &b) > 0) reply.push_back(b);
        if (completion_fault && isdu && (d[0] == 0xf1 || d[0] == 0xff)) {
            completion_retry_frame = frames.back();
            if (completion_fault == 1) reply.clear();
            else { CHECK(!reply.empty()); reply.back() ^= 1; }
            completion_fault = 0;
        }
        return rc;
    }
    void poll(lwIOLink::SinglePort &p) { p.poll(now++); }
    void boot(lwIOLink::SinglePort &p) {
        CHECK(p.start() == 0);
        for (unsigned i = 0; i < 20000 && p.state() != IOLINK_MASTER_STATE_OPERATE; ++i) poll(p);
        CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
    }
    void result(lwIOLink::SinglePort &p) {
        for (unsigned i = 0; i < 250000 && !p.result().ready; ++i) poll(p);
        CHECK(p.result().ready && !violation);
    }
    void tick(iolink_master_port_t &p) {
        iolink_master_timing_t timing{};
        CHECK(iolink_master_get_timing(&p, &timing) == 0);
        auto event = timing.awaiting_response && int32_t(now - timing.response_deadline_100us) >= 0
                         ? IOLINK_MASTER_TICK_RESPONSE_TIMEOUT : IOLINK_MASTER_TICK_CYCLE_DUE;
        iolink_master_tick_at(&p, event, now++);
    }
};
Transport *Transport::active = nullptr;

static void slow_response() {
    for (uint8_t od : {uint8_t(1), uint8_t(2)}) {
        for (uint32_t start : {0U, 0xffff8000U}) {
            Transport t(od);
            t.now = start;
            lwIOLink::SinglePort p(&t.api, t.config());
            t.boot(p);
            CHECK(p.cycleTime() == 320);
            uint8_t value[232];
            for (unsigned i = 0; i < sizeof(value); ++i) value[i] = uint8_t(i);
            fake_iolink_device_set_isdu_object(0x1234, 7, value, sizeof(value));
            t.busy_ticks = 45000; // Start responding within 5 s; finish after it.
            const auto began = t.now;
            CHECK(p.requestRead(0x1234, 7) == 0);
            t.result(p);
            CHECK(p.result().code == 0 && p.result().size == sizeof(value));
            CHECK(!std::memcmp(p.result().data, value, sizeof(value)));
            CHECK(uint32_t(t.now - began) > 50000);
            CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
        }
    }
}

static void completion_case(bool abort, uint8_t od, int fault, uint8_t pd) {
    Transport t(od, 0x80, pd);
    auto cfg = t.config();
    cfg.isdu_timeout_100us = 1000;
    lwIOLink::SinglePort p(&t.api, cfg);
    t.boot(p);
    uint8_t value[] = {7, 8, 9};
    fake_iolink_device_set_isdu_object(0x1234, 7, value, sizeof(value));
    // Chain read -> read -> write -> read with no extra poll after completion.
    for (unsigned i = 0; i < 4; ++i) {
        fake_iolink_device_malformed_isdu(abort && i == 0);
        size_t first = t.frames.size();
        CHECK((i == 2 ? p.requestWrite(0x1234, 7, value, sizeof(value))
                      : p.requestRead(0x1234, 7)) == 0);
        t.result(p);
        if (p.result().code != (abort && i == 0 ? IOLINK_MASTER_ISDU_ERR_DEVICE : 0))
            std::fprintf(stderr, "completion abort=%d od=%u pd=%u fault=%d operation=%u code=%d error=%04x\n",
                         abort, od, pd, fault, i, p.result().code, p.result().device_error);
        CHECK(p.result().code == (abort && i == 0 ? IOLINK_MASTER_ISDU_ERR_DEVICE : 0));
        if (i > 0) CHECK(t.frames[first][0] == (abort && i == 1 ? 0xff : 0xf1));
        if (i == 0) t.completion_fault = fault;
    }
    // A device error also needs IDLE before the following write.
    CHECK(p.requestRead(0x7777, 0) == 0);
    t.result(p);
    CHECK(p.result().code == IOLINK_MASTER_ISDU_ERR_DEVICE);
    size_t first = t.frames.size();
    CHECK(p.requestWrite(0x1234, 7, value, sizeof(value)) == 0);
    t.result(p);
    CHECK(p.result().code == 0 && t.frames[first][0] == 0xf1);
    CHECK(t.completion_retries == (fault ? 1U : 0U));
}

static void completion(bool abort) {
    for (uint8_t od : {uint8_t(1), uint8_t(2)})
        for (int fault : {0, 1, 2}) completion_case(abort, od, fault, 1);
    for (int fault : {0, 1, 2}) completion_case(abort, 1, fault, 0);
}

static void busy_timeout() {
    for (uint32_t start : {0U, 0xffffff00U}) {
        Transport t;
        t.now = start;
        auto cfg = t.config();
        cfg.isdu_timeout_100us = 1000;
        lwIOLink::SinglePort p(&t.api, cfg);
        t.boot(p);
        uint8_t value = 42;
        fake_iolink_device_set_isdu_object(24, 0, &value, 1);
        t.busy_ticks = 100000;
        uint32_t began = t.now;
        CHECK(p.requestRead(24, 0) == 0);
        t.result(p);
        CHECK(p.result().code == IOLINK_MASTER_ISDU_ERR_DEVICE);
        CHECK(p.result().device_error == 0x1100 && uint32_t(t.now - began) < 5000);
        t.busy_ticks = 0;
        size_t first = t.frames.size();
        CHECK(p.requestRead(24, 0) == 0);
        t.result(p);
        CHECK(p.result().code == 0 && p.result().data[0] == value);
        CHECK(t.frames[first][0] == 0xff);
    }
}

static void write_lengths() {
    struct Address { uint16_t index; uint8_t sub, width; };
    for (auto address : {Address{24, 0, 1}, Address{24, 7, 2}, Address{0x1234, 7, 3}}) {
        Transport t(1, 20);
        auto cfg = t.config();
        iolink_master_port_t core{};
        CHECK(iolink_master_init(&core, &t.api, &cfg) == 0);
        for (unsigned i = 0; i < 20000 && iolink_master_get_state(&core) != IOLINK_MASTER_STATE_OPERATE; ++i)
            t.tick(core);
        CHECK(iolink_master_get_state(&core) == IOLINK_MASTER_STATE_OPERATE);
        uint8_t data[238];
        for (unsigned i = 0; i < sizeof(data); ++i) data[i] = uint8_t(i ^ 0x5a);
        const uint8_t boundary = uint8_t(14 - address.width);
        const uint8_t maximum = uint8_t(235 - address.width);
        for (uint8_t len : {uint8_t(boundary - 1), boundary, uint8_t(boundary + 1), maximum}) {
            size_t first = t.frames.size();
            int rc = iolink_master_write_isdu(&core, address.index, address.sub, data, len);
            CHECK(rc == IOLINK_MASTER_STATUS_PENDING);
            for (unsigned i = 0; i < 100000 && rc == IOLINK_MASTER_STATUS_PENDING; ++i) {
                t.tick(core);
                rc = iolink_master_write_isdu(&core, address.index, address.sub, data, len);
            }
            // The fixture's object storage is limited to 232 data bytes. The
            // core still emits legal 238-byte PDUs for shorter index formats.
            CHECK(rc == (len > 232 ? IOLINK_MASTER_ISDU_ERR_DEVICE : 0) && !t.violation);
            std::vector<uint8_t> wire;
            for (size_t i = first; i < t.frames.size(); ++i) {
                const auto &f = t.frames[i];
                if ((f[0] & 0xe0) == 0x60) wire.insert(wire.end(), f.begin() + 2, f.end());
            }
            const bool extended = len >= boundary;
            const size_t expected = 2 + address.width + len + (extended ? 1 : 0);
            CHECK(wire.size() == expected);
            CHECK(wire[0] == uint8_t((address.width << 4) | (extended ? 1 : expected)));
            if (extended) CHECK(wire[1] == expected);
            size_t pos = extended ? 2 : 1;
            if (address.width == 3) CHECK(wire[pos++] == uint8_t(address.index >> 8));
            CHECK(wire[pos++] == uint8_t(address.index));
            if (address.width > 1) CHECK(wire[pos++] == address.sub);
            CHECK(!std::memcmp(&wire[pos], data, len));
            uint8_t chk = 0;
            for (auto b : wire) chk ^= b;
            CHECK(chk == 0);
        }
        // Drain the last completion before checking invalid length arguments.
        for (unsigned i = 0; i < 100; ++i) t.tick(core);
        CHECK(iolink_master_write_isdu(&core, address.index, address.sub, data, uint8_t(maximum + 1)) ==
              IOLINK_MASTER_ISDU_ERR_BUFFER_TOO_SMALL);
    }
}

static void segment_timeout() {
    Transport t(1, 20);
    lwIOLink::SinglePort p(&t.api, t.config());
    t.boot(p);
    uint8_t value[32]{};
    fake_iolink_device_set_isdu_object(0x1234, 7, value, sizeof(value));
    t.drop_segments = true;
    CHECK(p.requestRead(0x1234, 7) == 0);
    t.result(p);
    CHECK(p.result().code == lwIOLink::SinglePort::LinkLost);
    CHECK(t.retries == 2 && p.diagnostics().response_timeouts == 3);
    t.drop_segments = false;
    t.retry_frame.clear();
    for (unsigned i = 0; i < 20000 && p.state() != IOLINK_MASTER_STATE_OPERATE; ++i) t.poll(p);
    CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
    CHECK(p.requestRead(0x1234, 7) == 0);
    t.result(p);
    CHECK(p.result().code == 0 && p.result().size == sizeof(value));
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!std::strcmp(argv[1], "slow_response")) slow_response();
    else if (!std::strcmp(argv[1], "busy_timeout")) busy_timeout();
    else if (!std::strcmp(argv[1], "completion_idle")) completion(false);
    else if (!std::strcmp(argv[1], "completion_abort")) completion(true);
    else if (!std::strcmp(argv[1], "write_lengths")) write_lengths();
    else if (!std::strcmp(argv[1], "segment_timeout")) segment_timeout();
    else CHECK(false);
    std::printf("PASS: %s\n", argv[1]);
}
