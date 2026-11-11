// ethernet.cpp — see ethernet.h.
//
// T-Display P4 (LilyGo's t_display_p4/v1/config.h, the V1.0 schematic): an
// IC+ IP101 PHY at address 1 on the P4's EMAC over RMII, on the chip's
// primary RMII pads — TX_EN 49, TXD0 34, TXD1 35, CRS_DV 28, RXD0 29,
// RXD1 30, the PHY's 50 MHz reference clock INTO GPIO 50, MDC 31, MDIO 52;
// Arduino's P4 defaults (libraries/Ethernet/src/ETH.h) are the same pins.
// The PHY's reset is the expander line TDP4_IO_ETH_RST (active LOW), held
// LOW by the board layer until ethernet_begin(). GPIO35 is also the BOOT
// button: while the EMAC runs the pad is its TXD1 output, so the button's
// click interrupt is detached (input_dev_wake_pin_release) and the button
// is not an input; standby and power-off stop Ethernet first, which gives
// the pad back for the wake arm.

#include "ethernet.h"

#include <Arduino.h>
#include <WiFi.h>
#include "meshpunk_sync.h"   // SLog

#if defined(BOARD_TDISPLAY_P4)

#include <ETH.h>
#include "boards/tdisplay_p4_board.h"   // tdp4_exp_set, TDP4_IO_ETH_RST
#include "input/input_dev.h"            // input_dev_wake_pin_release/restore

#define ETH_PHY_ADDRESS   1
#define ETH_MDC_GPIO      31
#define ETH_MDIO_GPIO     52
#define ETH_ROUTE_PRIO    110   // the WiFi station's is 100 (esp_netif_defaults.h)

static bool s_started = false;
static bool s_events_hooked = false;

bool ethernet_available(void) { return true; }

bool ethernet_begin(void) {
  if (s_started) return true;
  if (!s_events_hooked) {
    Network.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
      (void)info;
      switch (event) {
        case ARDUINO_EVENT_ETH_START:        SLog.println("[ETH] started"); break;
        case ARDUINO_EVENT_ETH_CONNECTED:
          SLog.printf("[ETH] link up (%s duplex)\n", ETH.fullDuplex() ? "full" : "half");
          break;
        case ARDUINO_EVENT_ETH_GOT_IP:
          SLog.printf("[ETH] got IP %s, gateway %s, DNS %s\n",
                      ETH.localIP().toString().c_str(),
                      ETH.gatewayIP().toString().c_str(),
                      ETH.dnsIP().toString().c_str());
          break;
        case ARDUINO_EVENT_ETH_DISCONNECTED: SLog.println("[ETH] link down"); break;
        case ARDUINO_EVENT_ETH_STOP:         SLog.println("[ETH] stopped"); break;
        default: break;
      }
    });
    s_events_hooked = true;
  }
  input_dev_wake_pin_release();                 // GPIO35 becomes TXD1
  tdp4_exp_set(TDP4_IO_ETH_RST, true);          // PHY out of reset
  delay(30);                                    // the IP101 answers on MDIO after its reset
  // P4: eth_clock_mode_t is IDF's emac_rmii_clock_mode_t; the reference
  // clock comes in from the PHY on the default-config pin (GPIO 50).
  if (!ETH.begin(ETH_PHY_IP101, ETH_PHY_ADDRESS, ETH_MDC_GPIO, ETH_MDIO_GPIO, -1, EMAC_CLK_EXT_IN)) {
    SLog.println("[ETH] FAIL: EMAC/PHY start (the Arduino log line above has the reason)");
    tdp4_exp_set(TDP4_IO_ETH_RST, false);
    input_dev_wake_pin_restore();
    return false;
  }
  ETH.setRoutePrio(ETH_ROUTE_PRIO);
  s_started = true;
  SLog.println("[ETH] up: IP101 at address 1, reference clock in on GPIO 50; the BOOT button is TXD1 until Ethernet stops");
  return true;
}

void ethernet_end(void) {
  if (!s_started) return;
  ETH.end();                                    // EMAC down; its pads return to GPIO
  tdp4_exp_set(TDP4_IO_ETH_RST, false);         // PHY in reset = powered down
  s_started = false;
  input_dev_wake_pin_restore();                 // GPIO35 is the BOOT button again
  SLog.println("[ETH] down: PHY in reset, BOOT button restored");
}

bool      ethernet_started(void)   { return s_started; }
bool      ethernet_link(void)      { return s_started && ETH.linkUp(); }
bool      ethernet_connected(void) { return s_started && ETH.hasIP(); }
IPAddress ethernet_ip(void)        { return s_started ? ETH.localIP() : IPAddress(); }

#else  // boards without a wired port

bool      ethernet_available(void) { return false; }
bool      ethernet_begin(void)     { return false; }
void      ethernet_end(void)       {}
bool      ethernet_started(void)   { return false; }
bool      ethernet_link(void)      { return false; }
bool      ethernet_connected(void) { return false; }
IPAddress ethernet_ip(void)        { return IPAddress(); }

#endif

bool net_is_up(void) {
  return ethernet_connected() || WiFi.status() == WL_CONNECTED;
}

IPAddress net_local_ip(void) {
  if (ethernet_connected()) return ethernet_ip();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
  return IPAddress();
}
