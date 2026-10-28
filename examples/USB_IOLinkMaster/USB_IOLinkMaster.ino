#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include <SinglePort.hpp>
#include <UsbProtocol.hpp>
#include <driver/phy/tiol112/tiol112.hpp>

#if !ARDUINO_USB_MODE || !ARDUINO_USB_CDC_ON_BOOT
#error Select Hardware CDC and JTAG, and USB CDC On Boot Enabled.
#endif

using lwIOLink::driver::phy::TIOL112;
static TIOL112::tiol_cfg pins() {
    TIOL112::tiol_cfg c;
#if CONFIG_IDF_TARGET_ESP32C3
    c.tx_pin = 21;
    c.rx_pin = 20;
    c.tx_en = 4;
    c.fault_pin = 5; // XIAO D6,D7,D2,D3
#elif CONFIG_IDF_TARGET_ESP32S3
    c.tx_pin = 17;
    c.rx_pin = 18;
    c.tx_en = 16;
    c.fault_pin = 15; // ESP32-S3-DevKitC-1
#else
#error This example supports ESP32-C3 and ESP32-S3 only.
#endif
    return c;
}
static const auto pin_config = pins();
static TIOL112 phy(&pin_config);
static QueueHandle_t input_queue, output_queue;
static TaskHandle_t worker;
static esp_timer_handle_t timer;
static std::atomic<uint32_t> epoch{0};
struct Input {
    uint32_t epoch;
    char byte;
};
struct Output {
    uint32_t epoch;
    char text[lwIOLink::UsbProtocol::MaxReply];
};

static void timerWake(void *) {
    if (worker)
        xTaskNotifyGive(worker);
}
static void protocolTask(void *) {
    iolink_master_config_t cfg{};
    cfg.port_mode = IOLINK_MASTER_PORT_MODE_IOLINK;
    cfg.baudrate = IOLINK_BAUDRATE_COM3;
    cfg.auto_baudrate = true;
    cfg.m_seq_type = IOLINK_MASTER_M_SEQ_TYPE_0;
    cfg.auto_configure = true;
    phy.begin(); // Drive EN low before accepting commands; start() reports init errors.
    phy.configureHooks(cfg);
    lwIOLink::SinglePort port(phy.api(), cfg);
    lwIOLink::UsbProtocol protocol(port);
    uint32_t seen_epoch = epoch.load();
    // start is explicit: power-up keeps the C/Q driver disabled.
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t current = epoch.load();
        if (current != seen_epoch) {
            protocol.disconnect();
            seen_epoch = current;
        }
        port.poll(uint32_t(esp_timer_get_time() / 100));
        protocol.poll();
        Input in;
        for (unsigned n = 0;
             n < 32 && !protocol.hasReply() && xQueueReceive(input_queue, &in, 0) == pdTRUE; ++n)
            if (in.epoch == seen_epoch)
                protocol.feed(in.byte);
        if (protocol.hasReply()) {
            Output out{};
            out.epoch = seen_epoch;
            strlcpy(out.text, protocol.reply(), sizeof(out.text));
            if (xQueueSend(output_queue, &out, 0) == pdTRUE)
                protocol.consumeReply();
        }
    }
}
void setup() {
    Serial.setRxBufferSize(1024);
    Serial.setTxBufferSize(2048);
    Serial.setTxTimeoutMs(0);
    Serial.begin(115200);
    Serial.onEvent(ARDUINO_HW_CDC_BUS_RESET_EVENT, [](void *, esp_event_base_t, int32_t, void *) {
        epoch.fetch_add(1); // invalidate queued transport bytes on every USB bus reset
    });
    input_queue = xQueueCreate(1024, sizeof(Input));
    output_queue = xQueueCreate(2, sizeof(Output));
    if (!input_queue || !output_queue)
        return;
    if (xTaskCreate(protocolTask, "iolink", 12288, nullptr, configMAX_PRIORITIES - 3, &worker) !=
        pdPASS)
        return;
    esp_timer_create_args_t args{};
    args.callback = timerWake;
    args.name = "iolink100us";
    if (esp_timer_create(&args, &timer) == ESP_OK)
        esp_timer_start_periodic(timer, 100);
}
void loop() {
    static bool connected = false;
    static Output pending{};
    static size_t offset = 0;
    const bool now = bool(Serial);
    if (now != connected) {
        epoch.fetch_add(1);
        pending.text[0] = 0;
        offset = 0;
        connected = now;
    }
    if (!input_queue || !output_queue || !worker) {
        delay(10);
        return;
    }
    if (pending.text[0] && pending.epoch != epoch.load()) {
        pending.text[0] = 0;
        offset = 0;
    }
    if (connected) {
        for (unsigned n = 0; n < 64 && Serial.available() && uxQueueSpacesAvailable(input_queue);
             ++n) {
            Input in{epoch.load(), char(Serial.read())};
            xQueueSend(input_queue, &in, 0);
        }
        if (!pending.text[0]) {
            while (xQueueReceive(output_queue, &pending, 0) == pdTRUE) {
                if (pending.epoch == epoch.load())
                    break;
                pending.text[0] = 0;
            }
        }
        if (pending.text[0]) {
            const size_t remaining = strlen(pending.text) - offset;
            const size_t capacity = Serial.availableForWrite();
            if (capacity)
                offset += Serial.write(reinterpret_cast<const uint8_t *>(pending.text) + offset,
                                       remaining < capacity ? remaining : capacity);
            if (offset == strlen(pending.text)) {
                pending.text[0] = 0;
                offset = 0;
            }
        }
    }
    delay(1);
}
