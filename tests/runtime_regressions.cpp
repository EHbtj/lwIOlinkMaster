#include "SinglePort.hpp"
#include "UsbProtocol.hpp"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "fake_iolink_device.h"
#include "iolinki/crc.h"
#include "vendor/iolinki_master/master_internal.h"
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

struct Wire {
    static Wire *active;
    iolink_phy_api_t api{};
    uint32_t now = 0;
    uint8_t od, pd;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> reply, retry_frame, retry_reply;
    size_t pos = 0;
    int fault_mc = -1, fault_command = -1, faults = 0;
    bool drop = false, retry_pending = false;
    unsigned failures = 0, callbacks = 0, edges = 0;
    uint32_t busy_ticks = 0, busy_start = 0;
    bool busy_started = false;
    explicit Wire(uint8_t width = 1, uint8_t cycle = 20, uint8_t input = 1) : od(width), pd(input) {
        active = this;
        fake_iolink_device_reset(0xa5, pd, od);
        fake_iolink_device_set_direct_parameter_page1(cycle, od == 2 ? 11 : 1, pd ? 8 : 0, 0,
                                                      0x1234, 0x56789a);
        api = *fake_iolink_device_phy();
        api.user = this;
        api.send = [](void *p, const uint8_t *d, size_t n) {
            return static_cast<Wire *>(p)->send(d, n);
        };
        api.recv_byte = [](void *p, uint8_t *b) {
            auto &w = *static_cast<Wire *>(p);
            if (w.pos == w.reply.size())
                return 0;
            *b = w.reply[w.pos++];
            return 1;
        };
    }
    iolink_master_config_t config() {
        iolink_master_config_t c{};
        c.auto_configure = true;
        c.baudrate = IOLINK_BAUDRATE_COM3;
        c.flush_rx = []() {
            active->reply.clear();
            active->pos = 0;
            fake_iolink_device_flush_rx();
            return 0;
        };
        c.event_user = this;
        c.event_handler = [](void *p, const iolink_master_event_t *) {
            ++static_cast<Wire *>(p)->callbacks;
        };
        c.event_pending_handler = [](void *p) { ++static_cast<Wire *>(p)->edges; };
        return c;
    }
    int send(const uint8_t *d, size_t n) {
        frames.emplace_back(d, d + n);
        reply.clear();
        pos = 0;
        if (n == 1)
            retry_pending = false;
        if (retry_pending) {
            CHECK(retry_frame == frames.back());
            reply = retry_reply;
            retry_pending = false;
        } else if (d[0] == 0xf0 && busy_ticks &&
                   (!busy_started || uint32_t(now - busy_start) < busy_ticks)) {
            if (!busy_started) {
                busy_start = now;
                busy_started = true;
            }
            reply.assign(od, 0);
            reply[0] = 1;
            reply.insert(reply.end(), pd, 0xa5);
            reply.push_back(0);
            reply.back() = iolink_checksum6(reply.data(), reply.size());
        } else {
            CHECK(fake_iolink_device_phy()->send(nullptr, d, n) == int(n));
            uint8_t b;
            while (fake_iolink_device_phy()->recv_byte(nullptr, &b) > 0)
                reply.push_back(b);
        }
        if (faults && d[0] == fault_mc &&
            (fault_command < 0 || (n == 3 && d[2] == fault_command))) {
            --faults;
            ++failures;
            retry_frame = frames.back();
            retry_reply = reply;
            retry_pending = true;
            CHECK(!reply.empty());
            if (drop)
                reply.clear();
            else
                reply.back() ^= 1;
        }
        return int(n);
    }
    void poll(lwIOLink::SinglePort &p) { p.poll(now++); }
    void advance(lwIOLink::SinglePort &p, unsigned count = 1000) {
        while (count--)
            poll(p);
    }
    void boot(lwIOLink::SinglePort &p) {
        CHECK(p.start() == 0);
        for (unsigned n = 0; n < 20000 && p.state() != IOLINK_MASTER_STATE_OPERATE; ++n)
            poll(p);
        CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
    }
    void result(lwIOLink::SinglePort &p) {
        for (unsigned n = 0; n < 600000 && !p.result().ready; ++n)
            poll(p);
        CHECK(p.result().ready && p.result().code == 0);
        CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
    }
    void event(uint8_t mask = 0x3f) {
        uint8_t memory[19]{};
        memory[0] = 0x80 | mask;
        for (unsigned i = 0; i < 6; ++i) {
            memory[1 + 3 * i] = uint8_t(0xc4 | ((i % 3 + 1) << 4));
            memory[2 + 3 * i] = 0x77;
            memory[3 + 3 * i] = uint8_t(0x10 + i);
        }
        fake_iolink_device_set_event_memory(memory, sizeof(memory));
        fake_iolink_device_set_event_pending(true);
    }
    void settleEvents(lwIOLink::SinglePort &p) {
        advance(p, 10000);
        CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE && !p.diagnostics().event_pending);
        CHECK(p.lastEventError() == 0);
    }
};
Wire *Wire::active = nullptr;
static JsonDocument command(lwIOLink::UsbProtocol &usb, const char *request) {
    for (const char *s = request; *s; ++s)
        CHECK(usb.feed(*s));
    CHECK(usb.feed('\n') && usb.hasReply());
    CHECK(std::strlen(usb.reply()) < lwIOLink::UsbProtocol::MaxReply);
    JsonDocument result;
    CHECK(!deserializeJson(result, usb.reply()));
    CHECK(result["ok"] == true);
    usb.consumeReply();
    return result;
}
static void events_details() {
    for (auto profile : {0, 1, 2}) {
        Wire w(profile == 2 ? 2 : 1, 20, profile == 0 ? 0 : 1);
        w.now = 0xfffff000U;
        lwIOLink::SinglePort p(&w.api, w.config());
        w.boot(p);
        w.advance(p);
        const auto sample = p.sampleTime();
        w.frames.clear();
        w.event();
        w.settleEvents(p);
        CHECK(p.eventCount() == 6 && w.callbacks == 6 && w.edges == 1);
        CHECK(profile == 0 || p.sampleTime() != sample);
        unsigned reads = 0, writes = 0;
        for (const auto &f : w.frames) {
            if ((f[0] & 0xe0) == 0xc0)
                CHECK(unsigned(f[0] & 31) == reads++);
            if (f[0] == 0x40) {
                ++writes;
                CHECK(f[2] == 0);
                if (w.od == 2)
                    CHECK(f[3] == 0);
            }
        }
        CHECK(reads == 19 && writes == 1);
        lwIOLink::UsbProtocol usb(p);
        auto status = command(usb, "{\"id\":1,\"op\":\"status\"}");
        CHECK(status["events_available"] == 6 && status["event_pending"] == false);
        auto result = command(usb, "{\"id\":2,\"op\":\"events\"}");
        CHECK(result["events"].size() == 6 && result["remaining"] == 0 && result["dropped"] == 0);
        for (unsigned i = 0; i < 6; ++i) {
            CHECK(result["events"][i]["code"] == 0x7710 + i);
            CHECK(result["events"][i]["type"] == i % 3 + 1);
            CHECK(result["events"][i]["sequence"] == i + 1);
        }
        CHECK(p.eventCount() == 0);
    }
}
static void events_legacy() {
    Wire w(2);
    lwIOLink::SinglePort p(&w.api, w.config());
    w.boot(p);
    w.advance(p);
    const uint8_t status = 0x15;
    fake_iolink_device_set_event_memory(&status, 1);
    fake_iolink_device_set_event_pending(true);
    w.frames.clear();
    w.settleEvents(p);
    CHECK(p.eventCount() == 3);
    for (uint16_t code : {uint16_t(0xff10), uint16_t(0x6320), uint16_t(0xff80)}) {
        lwIOLink::SinglePort::Event e;
        CHECK(p.popEvent(e) && e.value.code == code && e.value.qualifier == 0x54);
    }
    unsigned reads = 0;
    for (const auto &f : w.frames)
        if ((f[0] & 0xe0) == 0xc0) {
            ++reads;
            CHECK(f[0] == 0xc0);
        }
    CHECK(reads == 1);
}
static void events_failures() {
    for (int mc : {0xc2, 0x40})
        for (bool drop : {false, true}) {
            Wire w(2);
            lwIOLink::SinglePort p(&w.api, w.config());
            w.boot(p);
            w.advance(p);
            w.fault_mc = mc;
            w.faults = 2;
            w.drop = drop;
            w.event(1);
            w.settleEvents(p);
            CHECK(w.failures == 2 && w.callbacks == 1 && p.eventCount() == 1);
            CHECK(fake_iolink_device_wakeup_count() == 1);
        }
    Wire w;
    lwIOLink::SinglePort p(&w.api, w.config());
    w.boot(p);
    w.advance(p);
    w.fault_mc = 0x40;
    w.faults = 3;
    w.event(1);
    for (unsigned n = 0; n < 10000 && p.state() == IOLINK_MASTER_STATE_OPERATE; ++n)
        w.poll(p);
    CHECK(p.state() == IOLINK_MASTER_STATE_STARTUP && p.eventCount() == 1 && w.callbacks == 1);
    CHECK(p.lastEventError() == lwIOLink::SinglePort::LinkLost);
    w.advance(p, 20000);
    CHECK(p.state() == IOLINK_MASTER_STATE_OPERATE);
    CHECK(p.eventCount() == 1 && w.callbacks == 1);
    w.event(1);
    w.settleEvents(p);
    CHECK(p.eventCount() == 2 && w.callbacks == 2);
}
static void events_overflow() {
    Wire w;
    lwIOLink::SinglePort p(&w.api, w.config());
    w.boot(p);
    w.advance(p);
    lwIOLink::UsbProtocol usb(p);
    for (char c : std::string("{\"id\":1,\"op\":\"status\"}\n"))
        CHECK(usb.feed(c));
    CHECK(usb.hasReply());
    const auto cycles = fake_iolink_device_operate_cycle_count();
    for (unsigned i = 0; i < 3; ++i) {
        w.event();
        w.settleEvents(p);
    }
    CHECK(p.eventCount() == 16 && p.droppedEvents() == 2 && w.callbacks == 18);
    CHECK(fake_iolink_device_operate_cycle_count() > cycles && usb.hasReply());
    usb.disconnect();
    CHECK(p.eventCount() == 16);
    auto result = command(usb, "{\"id\":2,\"op\":\"events\"}");
    CHECK(result["events"].size() == 6 && result["remaining"] == 10 && result["dropped"] == 2);
    CHECK(result["events"][0]["sequence"] == 3);
    p.stop();
    CHECK(p.eventCount() == 10);
}
static void events_isdu() {
    for (uint8_t od : {uint8_t(1), uint8_t(2)}) {
        Wire w(od);
        lwIOLink::SinglePort p(&w.api, w.config());
        w.boot(p);
        w.advance(p);
        uint8_t value[100];
        for (unsigned i = 0; i < 100; ++i)
            value[i] = uint8_t(i);
        fake_iolink_device_set_isdu_object(0x1234, 7, value, 100);
        CHECK(p.requestRead(0x1234, 7) == 0);
        bool injected = false;
        for (unsigned n = 0; n < 10000 && !p.result().ready; ++n) {
            w.poll(p);
            if (!injected && !w.frames.empty() && w.frames.back()[0] == 0xe1) {
                w.event();
                injected = true;
            }
        }
        CHECK(injected && p.result().ready && p.result().code == 0 && p.result().size == 100);
        CHECK(!std::memcmp(p.result().data, value, 100));
        w.settleEvents(p);
        CHECK(p.eventCount() == 6);
    }
}
static void slow_max_cycle() {
    for (uint8_t od : {uint8_t(1), uint8_t(2)})
        for (bool write : {false, true}) {
            Wire w(od, 0xbf);
            w.now = 0xffff0000U;
            lwIOLink::SinglePort p(&w.api, w.config());
            w.boot(p);
            CHECK(p.cycleTime() == 1328 && p.isduTimeout100us() > 300000);
            uint8_t value[232];
            for (unsigned i = 0; i < 232; ++i)
                value[i] = uint8_t(i);
            fake_iolink_device_set_isdu_object(0x1234, 7, value, 232);
            w.busy_ticks = 44000;
            const auto began = w.now;
            CHECK((write ? p.requestWrite(0x1234, 7, value, 232) : p.requestRead(0x1234, 7)) == 0);
            w.result(p);
            CHECK(uint32_t(w.now - began) < p.isduTimeout100us());
            if (od == 1)
                CHECK(uint32_t(w.now - began) > 300000);
            if (!write)
                CHECK(p.result().size == 232 && !std::memcmp(p.result().data, value, 232));
            lwIOLink::UsbProtocol usb(p);
            auto status = command(usb, "{\"id\":1,\"op\":\"status\"}");
            CHECK(status["isdu_timeout_us"].as<uint64_t>() == uint64_t(p.isduTimeout100us()) * 100);
        }
}
static void checksum_retries() {
    for (int mc : {0x20, 0x70, 0xf0, 0xf1})
        for (unsigned errors : {2U, 3U}) {
            Wire w(1, 20, mc == 0xf0 ? 0 : 1);
            lwIOLink::SinglePort p(&w.api, w.config());
            if (mc == 0x20) {
                w.fault_mc = mc;
                w.fault_command = 0x99;
                w.faults = errors;
            }
            w.boot(p);
            w.advance(p);
            if (mc == 0x20) {
                CHECK(w.failures == errors);
                CHECK(fake_iolink_device_wakeup_count() == (errors == 3 ? 2U : 1U));
                continue;
            }
            uint8_t value[] = {1, 2, 3};
            fake_iolink_device_set_isdu_object(0x1234, 7, value, 3);
            w.fault_mc = mc;
            w.faults = errors;
            const size_t first = w.frames.size();
            CHECK(p.requestRead(0x1234, 7) == 0);
            for (unsigned n = 0; n < 10000 && !p.result().ready; ++n)
                w.poll(p);
            if (mc == 0xf1)
                w.advance(p, 1000); // cyclic/previous completion may occur after service result
            CHECK(w.failures == errors);
            if (errors == 2) {
                CHECK(p.result().ready && p.result().code == 0 &&
                      fake_iolink_device_wakeup_count() == 1);
            } else if (mc != 0xf1) {
                CHECK(p.result().ready && p.result().code == lwIOLink::SinglePort::LinkLost);
            }
            unsigned attempts = 0;
            for (size_t i = first; i < w.frames.size(); ++i)
                if (w.frames[i][0] == mc)
                    ++attempts;
            CHECK(attempts >= 3);
        }
}
static void quality() {
    Wire w;
    auto config = w.config();
    iolink_master_port_t core{};
    CHECK(iolink_master_init(&core, &w.api, &config) == 0);
    auto *state = iolink_master_port_state(&core);
    for (uint32_t cycles : {0U, 42949672U, 42949673U, 50000000U, 0xffffffffU}) {
        state->cycle_count = cycles;
        iolink_master_diagnostics_t d{};
        CHECK(iolink_master_get_diagnostics(&core, &d) == 0 && d.link_quality_percent == 100);
    }
    state->diagnostics.checksum_errors = state->diagnostics.send_errors =
        state->diagnostics.response_timeouts = 0xffffffffU;
    iolink_master_diagnostics_t d{};
    CHECK(iolink_master_get_diagnostics(&core, &d) == 0 && d.link_quality_percent == 25);
}
static void event_c_api() {
    Wire w;
    auto config = w.config();
    iolink_master_port_t core{};
    CHECK(iolink_master_init(&core, &w.api, &config) == 0);
    auto tick = [&]() {
        iolink_master_timing_t t{};
        iolink_master_get_timing(&core, &t);
        const auto ev = t.awaiting_response && int32_t(w.now - t.response_deadline_100us) >= 0
                            ? IOLINK_MASTER_TICK_RESPONSE_TIMEOUT
                            : IOLINK_MASTER_TICK_CYCLE_DUE;
        iolink_master_tick_at(&core, ev, w.now++);
    };
    for (unsigned n = 0; n < 20000 && iolink_master_get_state(&core) != IOLINK_MASTER_STATE_OPERATE;
         ++n)
        tick();
    CHECK(iolink_master_get_state(&core) == IOLINK_MASTER_STATE_OPERATE);
    w.event();
    iolink_master_event_t events[6]{};
    uint8_t count = 0;
    CHECK(iolink_master_read_and_ack_events(&core, events, 0, &count) ==
          IOLINK_MASTER_ERR_INVALID_ARG);
    int rc = IOLINK_MASTER_STATUS_PENDING;
    for (unsigned n = 0; n < 10000 && rc == IOLINK_MASTER_STATUS_PENDING; ++n) {
        tick();
        rc = iolink_master_read_and_ack_events(&core, events, 1, &count);
    }
    CHECK(rc == IOLINK_MASTER_ERR_BUFFER_TOO_SMALL && count == 6 && events[0].code == 0);
    CHECK(iolink_master_read_and_ack_events(&core, events, 6, &count) == 0 && count == 6 &&
          w.callbacks == 6);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!std::strcmp(argv[1], "events_details"))
        events_details();
    else if (!std::strcmp(argv[1], "events_legacy"))
        events_legacy();
    else if (!std::strcmp(argv[1], "events_failures"))
        events_failures();
    else if (!std::strcmp(argv[1], "events_overflow"))
        events_overflow();
    else if (!std::strcmp(argv[1], "events_isdu"))
        events_isdu();
    else if (!std::strcmp(argv[1], "slow_max_cycle"))
        slow_max_cycle();
    else if (!std::strcmp(argv[1], "checksum_retries"))
        checksum_retries();
    else if (!std::strcmp(argv[1], "quality"))
        quality();
    else if (!std::strcmp(argv[1], "event_c_api"))
        event_c_api();
    else
        CHECK(false);
    std::printf("PASS: %s\n", argv[1]);
}
