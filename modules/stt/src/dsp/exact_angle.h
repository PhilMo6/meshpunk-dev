// exact_angle.h — the float that (float)((c * i) / d) gives in IEEE double
// arithmetic (c a double, i and d integers; round to nearest, ties to even,
// at the multiply, at the divide and at the conversion to float), computed
// with integer arithmetic: the builds without the ESP32-S3 assembly use it
// for the two tables the S3 build computes in double (fft4r_init's angles,
// the Hann window's), so both builds get the same tables; the RISC-V build
// has no double-precision hardware and links no libgcc.
// i < 2048, 0 < d < 65536, c a positive normal double.

#ifndef EXACT_ANGLE_H_
#define EXACT_ANGLE_H_

#include <cstdint>

namespace dsp
{
    inline constexpr uint64_t kPiBits{0x400921FB54442D18ull};      // M_PI
    inline constexpr uint64_t kTwoPiBits{0x401921FB54442D18ull};   // 2 * M_PI

    float exact_angle(uint64_t c_bits, uint32_t i, uint32_t d);
} // namespace dsp

#endif
