// gps_tdisplay_p4.cpp — LilyGo T-Display P4 GPS transport backend
// (contract: gps_dev.h).
//
// Quectel L76K (the Heltec's twin: gps_heltec.cpp) on UART1. LilyGo's v1
// config.h names the lines `l76k::kTx = 22` / `kRx = 23` and hands them to
// `HardwareUart(tx, rx, ...)` as `(kRx, kTx)` (cpp_bus_driver
// hardware_uart.h), so the names are the receiver's: its TXD is 22 and the
// P4 receives there, transmitting on 23. The firmware's auto-baud probe
// (main.cpp) is the check — NMEA characters counted at the module's rate
// confirm the pair; LilyGo's own init leaves a module at 115200 / 5 Hz,
// which the probe also covers.
//
// Wake: the receiver's wake line is expander IO11, driven HIGH by the board
// layer's power sequence (tdisplay_p4_board.cpp) before this backend runs —
// LilyGo's L76k::Sleep(enable) writes !enable to it: 1 runs the receiver,
// 0 parks it. The L76K has no controllable rail on this board, so
// gps_dev_power_down() / gps_dev_wake() drive that line (standby and power
// off, main.cpp).
//
// The L76K accepts standard PCAS/PMTK commands; bring-up keeps the factory
// defaults (NMEA at 9600, which the sync's baud probe locks onto).

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>

#include "gps_dev.h"
#include "../boards/tdisplay_p4_board.h"   // TDP4_IO_GPS_WAKE

#define P4_GPS_RX  22   // P4 RX  <- L76K TXD (LilyGo "kTx")
#define P4_GPS_TX  23   // P4 TX  -> L76K RXD (LilyGo "kRx")

static HardwareSerial GPSSerial(1);

void gps_dev_begin(uint32_t baud) {
  GPSSerial.begin(baud, SERIAL_8N1, P4_GPS_RX, P4_GPS_TX);
}

void gps_dev_update_baud(uint32_t baud) {
  GPSSerial.updateBaudRate(baud);
}

Stream& gps_dev_stream(void) { return GPSSerial; }

void gps_dev_chip_init(void) {}

void gps_dev_power_down(void) { tdp4_exp_set(TDP4_IO_GPS_WAKE, false); }
void gps_dev_wake(void)       { tdp4_exp_set(TDP4_IO_GPS_WAKE, true); }

int gps_dev_rx_pin(void) { return P4_GPS_RX; }
int gps_dev_tx_pin(void) { return P4_GPS_TX; }

#endif // BOARD_TDISPLAY_P4
