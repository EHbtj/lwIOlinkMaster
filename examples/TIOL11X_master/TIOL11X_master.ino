#include <Arduino.h>
#include <lwIOLinkMaster.hpp>

// Legacy PHY initialization example. See USB_IOLinkMaster for the full stack.
lwIOLink::driver::phy::TIOL112::tiol_cfg driver_cfg1 = [] {
    lwIOLink::driver::phy::TIOL112::tiol_cfg cfg;
#if CONFIG_IDF_TARGET_ESP32C3
    cfg.tx_pin = 21;
    cfg.rx_pin = 20;
    cfg.tx_en = 4;
    cfg.fault_pin = 5;
#elif CONFIG_IDF_TARGET_ESP32S3
    cfg.tx_pin = 17;
    cfg.rx_pin = 18;
    cfg.tx_en = 16;
    cfg.fault_pin = 15;
#endif
    return cfg;
}();

lwIOLink::driver::phy::TIOL112 Driver1(&driver_cfg1);
constexpr uint8_t total_ports = 1;

lwIOLink::Port Port1 = lwIOLink::Port(&Driver1, 1);

lwIOLink::Port *PortList[total_ports] = {&Port1};

lwIOLink::Master master(PortList, total_ports);

void setup(void) { const bool init = master.begin(); }

void loop() {}
