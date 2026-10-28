#include "SinglePort.hpp"
#include "UsbProtocol.hpp"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
extern "C" {
#include "fake_iolink_device.h"
#include "iolinki/crc.h"
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
static uint32_t now = 0;
static iolink_baudrate_t rate = IOLINK_BAUDRATE_COM3, target = IOLINK_BAUDRATE_COM3;
static bool unplugged = false, lose_isdu = false, waiting_retry = false, mismatched_retry = false;
static std::vector<uint8_t> lost_request, cached_response, replay;
static size_t replay_pos = 0;
static int transportSend(void *, const uint8_t *data, size_t len) {
    if (len == 1 && data[0] == 0x55)
        return fake_iolink_device_phy()->send(nullptr, data, len);
    if (unplugged || rate != target)
        return int(len);
    if (waiting_retry) {
        mismatched_retry |= lost_request != std::vector<uint8_t>(data, data + len);
        waiting_retry = false;
        replay = cached_response;
        replay_pos = 0;
        return int(len);
    }
    const int result = fake_iolink_device_phy()->send(nullptr, data, len);
    if (lose_isdu && (data[0] & 0x60) == 0x60) {
        lose_isdu = false;
        waiting_retry = true;
        lost_request.assign(data, data + len);
        cached_response.clear();
        uint8_t byte;
        while (fake_iolink_device_phy()->recv_byte(nullptr, &byte) > 0)
            cached_response.push_back(byte);
    }
    return result;
}
static int transportReceive(void *, uint8_t *byte) {
    if (unplugged)
        return 0;
    if (replay_pos < replay.size()) {
        *byte = replay[replay_pos++];
        return 1;
    }
    return fake_iolink_device_phy()->recv_byte(nullptr, byte);
}
static const iolink_phy_api_t *transport() {
    static iolink_phy_api_t api;
    api = *fake_iolink_device_phy();
    api.send = transportSend;
    api.recv_byte = transportReceive;
    api.set_baudrate = [](void *, iolink_baudrate_t b) { rate = b; };
    return &api;
}
static void advance(lwIOLink::SinglePort &port, unsigned count = 2000) {
    for (unsigned i = 0; i < count; ++i)
        port.poll(now++);
}
static std::string command(lwIOLink::UsbProtocol &usb, const std::string &s) {
    for (char c : s)
        CHECK(usb.feed(c));
    CHECK(usb.feed('\n'));
    CHECK(usb.hasReply());
    std::string reply = usb.reply();
    usb.consumeReply();
    return reply;
}
static iolink_master_config_t config() {
    iolink_master_config_t c{};
    c.auto_configure = true;
    c.auto_baudrate = true;
    c.m_seq_type = IOLINK_MASTER_M_SEQ_TYPE_0;
    c.baudrate = IOLINK_BAUDRATE_COM3;
    c.flush_rx = []() {
        fake_iolink_device_flush_rx();
        return 0;
    };
    return c;
}
static void fixture(uint8_t pd = 8, uint8_t cap = 1, uint8_t cycle = 20) {
    unplugged = false;
    lose_isdu = false;
    waiting_retry = false;
    mismatched_retry = false;
    lost_request.clear();
    cached_response.clear();
    replay.clear();
    replay_pos = 0;
    rate = target = IOLINK_BAUDRATE_COM3;
    now = 0;
    fake_iolink_device_reset(0xa5, uint8_t((pd & 128) ? (pd & 31) + 1 : (pd + 7) / 8),
                             cap == 11 ? 2 : 1);
    fake_iolink_device_set_direct_parameter_page1(cycle, cap, pd, 0, 0x1234, 0x56789a);
}
static void nominal() {
    fixture();
    auto cfg = config();
    lwIOLink::SinglePort port(fake_iolink_device_phy(), cfg);
    CHECK(port.start() == 0);
    advance(port);
    CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
    iolink_master_device_info_t info{};
    CHECK(port.deviceInfo(info) == 0);
    CHECK(info.vendor_id == 0x1234 && info.device_id == 0x56789a);
    uint8_t data[32], n = 0;
    CHECK(port.processData(data, sizeof(data), n) == 0 && n == 1 && data[0] == 0xa5);
    CHECK(port.processData(data, 0, n) == IOLINK_MASTER_ERR_BUFFER_TOO_SMALL && n == 1);
    const uint8_t value[] = {1, 2, 3, 4, 5};
    fake_iolink_device_set_isdu_object(0x1234, 2, value, sizeof(value));
    CHECK(port.requestRead(0x1234, 2) == 0);
    CHECK(port.requestRead(0x1234, 2) == lwIOLink::SinglePort::Busy);
    advance(port);
    CHECK(port.result().ready && port.result().code == 0);
    CHECK(port.result().size == 5 && !std::memcmp(port.result().data, value, 5));
    uint8_t replacement[] = {9, 8, 7};
    CHECK(port.requestWrite(0x1234, 2, replacement, 3) == 0);
    advance(port);
    CHECK(port.result().ready && port.result().code == 0);
    CHECK(port.requestRead(0x1234, 2) == 0);
    advance(port);
    CHECK(port.result().size == 3 && !std::memcmp(port.result().data, replacement, 3));
    CHECK(port.requestRead(0x7777, 0) == 0);
    advance(port);
    CHECK(port.result().ready && port.result().code < 0 && port.result().device_error != 0);
    fake_iolink_device_corrupt_next_response_checksum();
    advance(port);
    CHECK(port.diagnostics().checksum_errors > 0);
    fake_iolink_device_drop_next_response();
    advance(port);
    CHECK(port.diagnostics().response_timeouts > 0);
    fake_iolink_device_truncate_next_response();
    advance(port);
    CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
    lwIOLink::UsbProtocol usb(port);
    CHECK(command(usb, "{\"id\":1,\"op\":\"status\"}").find("operate") != std::string::npos);
    CHECK(command(usb, "{\"id\":2,\"op\":\"pd\"}").find("a5") != std::string::npos);
    CHECK(
        command(usb, "{\"id\":3,\"op\":\"isdu_write\",\"index\":1,\"subindex\":0,\"data\":\"xz\"}")
            .find("invalid_hex") != std::string::npos);
    CHECK(command(usb, "{\"id\":4,\"op\":\"isdu_read\",\"index\":65536,\"subindex\":0}")
              .find("invalid_index") != std::string::npos);
    CHECK(command(usb, "{\"id\":5,\"op\":\"unknown\"}").find("unknown_operation") !=
          std::string::npos);
    CHECK(command(usb, "{").find("invalid_request") != std::string::npos);
    CHECK(command(usb, std::string(800, 'x')).find("line_too_long") != std::string::npos);
    for (char c : std::string("{\"id\":6,\"op\":\"status\"}\n"))
        CHECK(usb.feed(c));
    CHECK(!usb.feed('x'));
    auto cycles = fake_iolink_device_operate_cycle_count();
    advance(port);
    CHECK(fake_iolink_device_operate_cycle_count() > cycles);
    usb.disconnect();
    CHECK(!usb.hasReply());
    CHECK(command(usb, "{\"id\":7,\"op\":\"status\"}").find("operate") != std::string::npos);
    port.stop();
    CHECK(port.state() == IOLINK_MASTER_STATE_INACTIVE);
}
static void profiles() {
    for (auto pd : {uint8_t(8), uint8_t(16), uint8_t(0x83), uint8_t(0x9f)}) {
        fixture(pd, pd < 128 ? 1 : 9, 0x80);
        auto cfg = config();
        lwIOLink::SinglePort port(fake_iolink_device_phy(), cfg);
        CHECK(port.start() == 0);
        advance(port, 20000);
        CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
        CHECK(port.cycleTime() >= 320);
    }
    fixture(8, 3);
    auto cfg = config();
    lwIOLink::SinglePort unsupported(fake_iolink_device_phy(), cfg);
    CHECK(unsupported.start() == 0);
    advance(unsupported);
    CHECK(unsupported.state() == IOLINK_MASTER_STATE_ERROR);
}
static void wraparound() {
    fixture();
    now = 0xfffffff0U;
    auto cfg = config();
    lwIOLink::SinglePort port(fake_iolink_device_phy(), cfg);
    CHECK(port.start() == 0);
    advance(port);
    CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
}
static void rates_and_recovery() {
    for (auto baud : {IOLINK_BAUDRATE_COM1, IOLINK_BAUDRATE_COM2, IOLINK_BAUDRATE_COM3}) {
        fixture();
        target = baud;
        auto cfg = config();
        lwIOLink::SinglePort port(transport(), cfg);
        CHECK(port.start() == 0);
        advance(port, 30000);
        if (port.state() != IOLINK_MASTER_STATE_OPERATE)
            std::fprintf(stderr, "rate=%u target=%u state=%u error=%d timeouts=%u wakes=%u\n", rate,
                         target, port.state(), port.lastError(),
                         port.diagnostics().response_timeouts, fake_iolink_device_wakeup_count());
        CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
        CHECK(rate == target);
        CHECK(fake_iolink_device_wakeup_count() == 1);
        if (baud == IOLINK_BAUDRATE_COM1)
            CHECK(port.cycleTime() >= 180);
        uint8_t value[232];
        for (unsigned i = 0; i < 232; ++i)
            value[i] = uint8_t(i);
        fake_iolink_device_set_isdu_object(0x2345, 7, value, 232);
        CHECK(port.requestRead(0x2345, 7) == 0);
        advance(port, 200000);
        CHECK(port.result().ready && port.result().code == 0 && port.result().size == 232);
        CHECK(!std::memcmp(port.result().data, value, 232));
        CHECK(port.requestWrite(0x2345, 7, value, 232) == 0);
        advance(port, 200000);
        CHECK(port.result().ready && port.result().code == 0);
        CHECK(port.requestRead(0x2345, 7) == 0);
        advance(port, 200000);
        CHECK(port.result().size == 232 && !std::memcmp(port.result().data, value, 232));
        lose_isdu = true;
        CHECK(port.requestRead(0x2345, 7) == 0);
        advance(port, 200000);
        CHECK(port.result().ready && port.result().code == 0 && !mismatched_retry);
        CHECK(port.requestRead(0x2345, 7) == 0);
        unplugged = true;
        advance(port, 20000);
        CHECK(port.result().ready && port.result().code == lwIOLink::SinglePort::LinkLost);
        iolink_master_device_info_t info{};
        CHECK(port.deviceInfo(info) != 0);
        unplugged = false;
        advance(port, 30000);
        CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
    }
}
static void server() {
    fixture();
    uint8_t value[] = {1, 2, 3};
    fake_iolink_device_set_isdu_object(0x1234, 0, value, 3);
    auto cfg = config();
    lwIOLink::SinglePort port(fake_iolink_device_phy(), cfg);
    lwIOLink::UsbProtocol usb(port);
    std::string line;
    while (std::getline(std::cin, line)) {
        for (char c : line)
            usb.feed(c);
        usb.feed('\n');
        for (unsigned i = 0; i < 300001 && !usb.hasReply(); ++i) {
            port.poll(now++);
            usb.poll();
        }
        if (usb.hasReply()) {
            std::cout << usb.reply() << std::flush;
            usb.consumeReply();
        }
        advance(port);
    }
}
static void malformed_and_busy() {
    fixture();
    auto cfg = config();
    cfg.isdu_timeout_100us = 1000;
    lwIOLink::SinglePort port(fake_iolink_device_phy(), cfg);
    CHECK(port.start() == 0);
    advance(port);
    uint8_t data[32], size = 0;
    CHECK(port.processData(data, sizeof(data), size) == 0);
    fake_iolink_device_invalid_pd(true);
    advance(port);
    CHECK(port.processData(data, sizeof(data), size) == IOLINK_MASTER_STATUS_PENDING);
    fake_iolink_device_invalid_pd(false);
    advance(port);
    CHECK(port.processData(data, sizeof(data), size) == 0);
    uint8_t value[] = {1, 2};
    fake_iolink_device_set_isdu_object(24, 0, value, 2);
    fake_iolink_device_malformed_isdu(true);
    CHECK(port.requestRead(24, 0) == 0);
    advance(port);
    CHECK(port.result().ready && port.result().code < 0 && port.result().device_error == 0x5700);
    fake_iolink_device_malformed_isdu(false);
    fake_iolink_device_busy(true);
    CHECK(port.requestRead(24, 0) == 0);
    advance(port, 5000);
    CHECK(port.result().ready && port.result().device_error == 0x1100);
    CHECK(port.state() == IOLINK_MASTER_STATE_OPERATE);
    fake_iolink_device_busy(false);
    CHECK(port.requestRead(24, 0) == 0);
    advance(port);
    CHECK(port.result().ready && port.result().code == 0 && port.result().device_error == 0);
}
static void wire_vectors_and_json() {
    // Fixed vectors from the A.1.6 equations, independent of the fake device.
    const uint8_t probe[] = {0xa2, 0}, operate[] = {0x20, 0, 0x99}, flags[] = {0};
    CHECK(iolink_checksum6(probe, 2) == 0 && iolink_checksum6(operate, 3) == 6);
    CHECK(iolink_checksum6(flags, 1) == 0x2d);
    fixture();
    auto cfg = config();
    lwIOLink::SinglePort port(fake_iolink_device_phy(), cfg);
    lwIOLink::UsbProtocol usb(port);
    CHECK(command(usb, "{\"id\":1,\"op\":\"start\"}garbage").find("invalid_request") !=
          std::string::npos);
    CHECK(port.state() == IOLINK_MASTER_STATE_INACTIVE);
    CHECK(command(usb, "{\"id\":1,\"op\":\"status\"}\r").find("inactive") != std::string::npos);
    CHECK(command(usb, "{\"id\":1,\"op\":\"sta\rrt\"}").find("invalid_request") !=
          std::string::npos);
}
int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--server")) {
        server();
        return 0;
    }
    nominal();
    profiles();
    wraparound();
    rates_and_recovery();
    malformed_and_busy();
    wire_vectors_and_json();
    std::puts("PASS: startup, identity, PD, ISDU read/write/error, checksum, timeout, truncation, "
              "USB framing/backpressure, profiles, clock wrap");
}
