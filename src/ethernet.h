// ethernet.h — wired Ethernet (the T-Display P4's IP101 PHY on the chip's
// EMAC) and the "is the device online" answer shared by WiFi and Ethernet.
//
// Boards without a wired port: ethernet_available() is false and the
// start/stop calls do nothing, so main.cpp's lifecycle hooks are
// unconditional.

#pragma once

#include <stdint.h>
#include <IPAddress.h>

bool      ethernet_available(void);   // this board has a wired port
bool      ethernet_begin(void);       // PHY out of reset + EMAC up; false = not started (reason logged)
void      ethernet_end(void);         // EMAC down + PHY in reset
bool      ethernet_started(void);     // begin() succeeded and end() has not run
bool      ethernet_link(void);        // cable link up
bool      ethernet_connected(void);   // link up with an IP address
IPAddress ethernet_ip(void);

// Online = Ethernet with an IP address, or the WiFi station connected. The
// default route follows the same order (Ethernet's route priority is set
// above the WiFi station's).
bool      net_is_up(void);
IPAddress net_local_ip(void);
