// rtc_dev.h — per-device hardware real-time-clock backend contract.
//
// One backend per build via the board define. The firmware's clock is the
// host clock (host_rtc in main.cpp, millis-driven, set through
// meshpunk_set_clock()); a board with a clock chip seeds it at boot and
// receives every accepted set of phone tier or better back, so the next
// boot starts from the chip's time. Boards without a clock chip build
// rtc_none.cpp: read() is false and write() does nothing.
// T-Display P4: NXP PCF8563 on the board layer's I2C bus
// (rtc_tdisplay_p4.cpp).

#pragma once

#include <stdint.h>

// The chip's current time as a UTC epoch. False when the board has no
// clock chip, the chip does not answer, its clock is halted, or it reports
// that the stored time is not trustworthy (supply dropped since the last
// write).
bool rtc_dev_read(uint32_t* epoch);

// Write a UTC epoch into the chip. False when the board has no clock chip,
// the epoch is outside the chip's range, or the write failed.
bool rtc_dev_write(uint32_t epoch);
