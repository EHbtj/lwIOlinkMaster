#include "UsbProtocol.hpp"
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>
namespace lwIOLink {
// ArduinoJson may stop at the first object. Require a whole NDJSON request,
// so trailing garbage cannot turn a malformed write into an accepted command.
static bool singleObject(const char *text, size_t size) {
    size_t first = 0;
    while (first < size && (text[first] == ' ' || text[first] == '\t'))
        ++first;
    if (first == size || text[first] != '{')
        return false;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = first; i < size; ++i) {
        const char c = text[i];
        if (quoted) {
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            if (!depth)
                return false;
            if (--depth == 0) {
                while (++i < size)
                    if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r')
                        return false;
                return true;
            }
        }
    }
    return false;
}
static void hex(const uint8_t *data, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * size] = 0;
}
static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static void encode(const JsonDocument &doc, char *out, size_t size) {
    if (measureJson(doc) + 2 > size) {
        std::snprintf(out, size, "{\"id\":null,\"ok\":false,\"error\":\"response_too_large\"}\n");
        return;
    }
    const auto n = serializeJson(doc, out, size - 2);
    out[n] = '\n';
    out[n + 1] = 0;
}
void UsbProtocol::error(uint32_t id, bool valid, const char *message, int code, uint16_t device) {
    JsonDocument d;
    if (valid)
        d["id"] = id;
    else
        d["id"] = nullptr;
    d["ok"] = false;
    d["error"] = message;
    d["code"] = code;
    d["device_error"] = device;
    encode(d, reply_, sizeof(reply_));
}
bool UsbProtocol::feed(char c) {
    if (hasReply())
        return false;
    if (c == '\n') {
        if (overflow_)
            error(0, false, "line_too_long");
        else if (used_) {
            line_[used_] = 0;
            dispatch();
        }
        used_ = 0;
        overflow_ = false;
        return true;
    }
    if (c == 0)
        overflow_ = true;
    if (!overflow_) {
        if (used_ == MaxLine)
            overflow_ = true;
        else
            line_[used_++] = c;
    }
    return true;
}
void UsbProtocol::disconnect() {
    used_ = 0;
    overflow_ = false;
    consumeReply();
    waiting_ = false;
}
void UsbProtocol::poll() {
    if (waiting_ && !hasReply() && port_.result().ready) {
        const auto &r = port_.result();
        if (r.code)
            error(waiting_id_, true, "isdu_failed", r.code, r.device_error);
        else {
            JsonDocument d;
            d["id"] = waiting_id_;
            d["ok"] = true;
            char bytes[SinglePort::MaxIsdu * 2 + 1];
            hex(r.data, r.size, bytes);
            d["data"] = bytes;
            encode(d, reply_, sizeof(reply_));
        }
        waiting_ = false;
    }
}
void UsbProtocol::dispatch() {
    if (!singleObject(line_, used_)) {
        error(0, false, "invalid_request");
        return;
    }
    JsonDocument request;
    const auto err = deserializeJson(request, line_, used_, DeserializationOption::NestingLimit(2));
    if (err || !request.is<JsonObject>() || !request["id"].is<uint32_t>() ||
        !request["op"].is<const char *>()) {
        error(0, false, "invalid_request");
        return;
    }
    const uint32_t id = request["id"];
    const char *op = request["op"];
    if (request["op"].as<JsonString>().size() != std::strlen(op)) {
        error(id, true, "invalid_request");
        return;
    }
    const bool rd = std::strcmp(op, "isdu_read") == 0, wr = std::strcmp(op, "isdu_write") == 0;
    for (JsonPair pair : request.as<JsonObject>()) {
        const char *k = pair.key().c_str();
        if (std::strcmp(k, "id") && std::strcmp(k, "op") &&
            !((rd || wr) && (!std::strcmp(k, "index") || !std::strcmp(k, "subindex"))) &&
            !(wr && !std::strcmp(k, "data"))) {
            error(id, true, "unknown_field");
            return;
        }
    }
    if (rd || wr) {
        if (waiting_ || port_.busy()) {
            error(id, true, "busy", SinglePort::Busy);
            return;
        }
        if (!request["index"].is<uint16_t>() || !request["subindex"].is<uint8_t>()) {
            error(id, true, "invalid_index");
            return;
        }
        int rc;
        if (wr) {
            if (!request["data"].is<const char *>()) {
                error(id, true, "invalid_hex");
                return;
            }
            const char *s = request["data"];
            size_t n = std::strlen(s);
            if (n != request["data"].as<JsonString>().size() || !n || n % 2 ||
                n > SinglePort::MaxIsdu * 2) {
                error(id, true, "invalid_hex");
                return;
            }
            uint8_t bytes[SinglePort::MaxIsdu];
            for (size_t i = 0; i < n / 2; ++i) {
                const int a = nibble(s[i * 2]), b = nibble(s[i * 2 + 1]);
                if (a < 0 || b < 0) {
                    error(id, true, "invalid_hex");
                    return;
                }
                bytes[i] = uint8_t(a * 16 + b);
            }
            rc = port_.requestWrite(request["index"], request["subindex"], bytes, n / 2);
        } else
            rc = port_.requestRead(request["index"], request["subindex"]);
        if (rc)
            error(id, true, "isdu_rejected", rc);
        else {
            waiting_ = true;
            waiting_id_ = id;
        }
        return;
    }
    JsonDocument d;
    d["id"] = id;
    d["ok"] = true;
    if (!std::strcmp(op, "start")) {
        int rc = port_.start();
        if (rc) {
            error(id, true, "start_failed", rc);
            return;
        }
    } else if (!std::strcmp(op, "stop"))
        port_.stop();
    else if (!std::strcmp(op, "status")) {
        static const char *states[] = {"inactive", "startup", "preoperate", "operate", "error"};
        d["state"] = states[unsigned(port_.state())];
        d["busy"] = port_.busy();
        d["protocol"] = 1;
        d["cycle_us"] = uint32_t(port_.cycleTime()) * 100;
        d["last_error"] = port_.lastError();
        auto diag = port_.diagnostics();
        d["checksum_errors"] = diag.checksum_errors;
        d["response_timeouts"] = diag.response_timeouts;
        d["cycle_slips"] = diag.cycle_slips;
        d["device_error"] = diag.last_isdu_error;
    } else if (!std::strcmp(op, "info")) {
        iolink_master_device_info_t info{};
        if (port_.deviceInfo(info) != 0) {
            error(id, true, "not_ready", SinglePort::NotReady);
            return;
        }
        d["vendor_id"] = info.vendor_id;
        d["device_id"] = info.device_id;
        d["revision"] = info.revision_id;
        d["pd_in_bytes"] = info.pd_in_len;
        d["isdu_supported"] = info.isdu_supported;
        d["mseq_capability"] = info.mseq_capability;
        d["min_cycle_us"] = uint32_t(info.min_cycle_time_100us) * 100;
    } else if (!std::strcmp(op, "pd")) {
        uint8_t bytes[32], n = 0;
        int rc = port_.processData(bytes, sizeof(bytes), n);
        if (rc) {
            error(id, true, "data_invalid", rc);
            return;
        }
        char encoded[65];
        hex(bytes, n, encoded);
        d["data"] = encoded;
        d["sample_time_100us"] = port_.sampleTime();
    } else {
        error(id, true, "unknown_operation");
        return;
    }
    encode(d, reply_, sizeof(reply_));
}
} // namespace lwIOLink
