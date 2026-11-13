// exact_angle.cpp — see exact_angle.h.

#include "exact_angle.h"

#include <cassert>
#include <cstring>

namespace
{
    int BitLength(uint64_t v)
    {
        int b = 0;
        while (v != 0)
        {
            b++;
            v >>= 1;
        }
        return b;
    }

    // v rounded to `bits` significant bits, to nearest, ties to even;
    // `sticky` = nonzero bits below v's lowest bit (an exact half then
    // rounds up). The result times 2^(*shift) is the rounded value.
    uint64_t RoundTo(const uint64_t v, const int bits, const bool sticky, int *shift)
    {
        const int len = BitLength(v);
        if (len <= bits)
        {
            assert(!sticky);
            *shift = 0;
            return v;
        }
        const int s = len - bits;
        uint64_t q = v >> s;
        const uint64_t dropped = v & ((uint64_t{1} << s) - 1);
        const uint64_t half = uint64_t{1} << (s - 1);
        *shift = s;
        if (dropped > half || (dropped == half && (sticky || (q & 1) != 0)))
        {
            q++;
            if (q == (uint64_t{1} << bits))
            {
                q >>= 1;
                *shift = s + 1;
            }
        }
        return q;
    }
} // namespace

namespace dsp
{
    float exact_angle(const uint64_t c_bits, const uint32_t i, const uint32_t d)
    {
        assert(i < 2048 && d > 0 && d < 65536);
        if (i == 0)
        {
            return 0.0f;
        }
        // c = m * 2^e, m the 53-bit significand.
        const uint64_t m = (c_bits & ((uint64_t{1} << 52) - 1)) | (uint64_t{1} << 52);
        int e = static_cast<int>((c_bits >> 52) & 0x7FF) - 1075;

        // x = c * i rounded to a double: x * 2^e, x of 53 bits (m * i has
        // 53 to 63 bits).
        int s = 0;
        const uint64_t x = RoundTo(m * i, 53, false, &s);
        e += s;

        // y = x / d rounded to a double: (x << k) / d by long division in
        // 16-bit limbs (the dividend has at most 78 bits), with k such that
        // the quotient has 62 or 63 bits; the remainder is the sticky bit.
        const int k = 9 + BitLength(d);
        const uint64_t lo = x << k;
        const uint32_t limbs[5] = {
            static_cast<uint32_t>(x >> (64 - k)),
            static_cast<uint32_t>(lo >> 48) & 0xFFFFu,
            static_cast<uint32_t>(lo >> 32) & 0xFFFFu,
            static_cast<uint32_t>(lo >> 16) & 0xFFFFu,
            static_cast<uint32_t>(lo) & 0xFFFFu,
        };
        uint64_t q = 0;
        uint32_t rem = 0;
        for (int t = 0; t < 5; t++)
        {
            const uint32_t cur = (rem << 16) | limbs[t];
            q = (q << 16) | (cur / d);
            rem = cur % d;
        }
        const uint64_t y = RoundTo(q, 53, rem != 0, &s);
        e = e - k + s;

        // y rounded to a float: f * 2^e, f of 24 bits.
        const uint64_t f = RoundTo(y, 24, false, &s);
        e += s;
        const int exponent = e + 23 + 127;
        assert(exponent > 0 && exponent < 255);
        const uint32_t bits = (static_cast<uint32_t>(exponent) << 23) | (static_cast<uint32_t>(f) & 0x7FFFFFu);
        float out;
        memcpy(&out, &bits, sizeof(out));
        return out;
    }
} // namespace dsp
