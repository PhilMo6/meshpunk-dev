// mic_none.cpp — mic_dev.h hardware layer for boards without a microphone
// (Heltec V4).

#include "mic_dev.h"

#if !defined(BOARD_TDECK) && !defined(BOARD_WIO_L2) && !defined(BOARD_TDISPLAY_P4)

MicDevKind mic_hw_kind(void)                       { return MIC_DEV_NONE; }
uint8_t    mic_hw_slot_bits(void)                  { return 16; }
uint8_t    mic_hw_inputs(void)                     { return 0; }
void       mic_hw_input_pos(uint8_t n, uint8_t* slot, uint8_t* shift) { (void)n; *slot = 0; *shift = 0; }
bool       mic_hw_start(uint32_t rate_hz, int* port) { (void)rate_hz; (void)port; return false; }
void       mic_hw_stop(void)                       {}
int        mic_hw_set_gain_step(uint8_t step)      { (void)step; return -1; }

#endif
