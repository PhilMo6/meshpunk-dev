// rtc_none.cpp — rtc_dev.h for boards without a hardware clock chip:
// nothing to read, nothing to write.

#if !defined(BOARD_TDISPLAY_P4)

#include "rtc_dev.h"

bool rtc_dev_read(uint32_t* epoch) { (void)epoch; return false; }
bool rtc_dev_write(uint32_t epoch) { (void)epoch; return false; }

#endif
