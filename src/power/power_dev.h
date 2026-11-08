// power_dev.h — per-device power/battery backend contract.
//
// One backend per build via the board define. Owns the peripheral power
// rail and the battery measurement path (divider ratio, ADC setup,
// averaging, or a PMU chip on boards that have one).

#pragma once

#include <stdint.h>

// Enable the board's peripheral power rail. Called EARLY in setup(), before
// any peripheral that needs the rail is touched.
void power_dev_init(void);

// Battery voltage in millivolts, or 0 on boards with no measurement path.
// The single battery read for the whole firmware — UI topbar, Lua
// (_get_battery_mv) and the BLE companion all come through here so they
// can never report differently-scaled values.
uint16_t power_dev_battery_mv(void);

// Board tail of the power-off path: park board-specific peripherals/rails,
// arm the wake button (GPIO0 on the S3 boards, active LOW), enter deep sleep.
// Never returns; waking is a full reboot (reset reason ESP_RST_DEEPSLEEP).
// T-Display P4: its button (GPIO35) cannot wake deep sleep, so it light-
// sleeps with GPIO35 armed and restarts on a press (reset reason ESP_RST_SW).
// The device-neutral teardown (store flush, radio off, BLE/WiFi stop, SD
// unmount) is system_shutdown() in main.cpp and has already run.
// power_dev_init() must release this path's RTC holds on the wake boot.
void power_dev_shutdown(void);

// Standby (light sleep) rail hooks. enter() parks what the board can afford
// to lose while the radio keeps receiving; exit() restores it.
// T-Deck: no-ops — the radio lives on the switched V3V rail (GPIO10 keeps
// driving through light sleep), and the Plus GPS is on the always-on rail
// with no power control. Heltec: VGNSS rail off/on (GPS).
void power_dev_standby_enter(void);
void power_dev_standby_exit(void);

// Per-sleep standby wake hooks, called by standby_run() under MESH_LOCK
// around each esp_light_sleep_start(), after the shared code armed the BOOT
// button and (on boards wiring it to a GPIO, PIN_LORA_DIO1 >= 0) DIO1.
// `radio` = a radio is running and armed in RX. arm() returns false when a
// received packet is already signalled: that round does not sleep, so the
// mesh task drains it first. T-Display P4: DIO1 is an expander input, so
// arm() reads the expander's input ports (releasing its INT line) and arms
// INT (GPIO5) as a level-LOW wake; disarm() disarms it. Other boards:
// arm() returns true, both do nothing.
bool power_dev_standby_arm(bool radio);
void power_dev_standby_disarm(bool radio);

// USB host port power. usb_core calls begin() when host mode starts, tick()
// from its task loop while host mode runs, and end() when host mode stops.
// begin() returns true when the board will supply 5 V on its USB-C port to
// an attached device. Boards with no port power control return false and
// do nothing in tick()/end(). Wio L2: the AW35615 port controller detects
// the device and OTG_EN (expander P13) switches VBUS (power_wio_l2.cpp).
bool power_dev_usb_host_begin(void);
void power_dev_usb_host_tick(void);
void power_dev_usb_host_end(void);

// True when host mode will supply 5 V on the USB-C port to an attached
// device (_device_caps().usb_power). Wio L2: the AW35615 answered at boot.
bool power_dev_usb_port_power(void);
