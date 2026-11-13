// profile.cpp — see profile.h.

#include "profile.h"

#ifdef STT_PROFILE

#include <cassert>

namespace stt_prof
{
    namespace
    {
        constexpr uint8_t kMaxDepth = 16;

        uint64_t g_charged[kStages];
        Stage g_stack[kMaxDepth];
        uint8_t g_depth = 0;   // g_stack[g_depth] runs
        uint32_t g_last = 0;   // the counter at the last charge

        const char *const kNames[kStages] = {
            "other", "sums", "int8 post", "f32 matmul", "quantize", "tile copy", "model read",
            "softmax", "norm", "glu", "dw conv", "conv2d", "transpose", "elementwise",
            "attn misc", "mel",
        };

        inline uint32_t Counter(void)
        {
            uint32_t c;
#if defined(__riscv)
            asm volatile("rdcycle %0" : "=r"(c));
#else
            asm volatile("rsr.ccount %0" : "=a"(c));
#endif
            return c;
        }

        inline void Charge(void)
        {
            const uint32_t now = Counter();
            g_charged[g_stack[g_depth]] += now - g_last;
            g_last = now;
        }
    } // namespace

    void Enter(const Stage s)
    {
        Charge();
        assert(g_depth + 1 < kMaxDepth);
        g_stack[++g_depth] = s;
    }

    void Leave(void)
    {
        Charge();
        assert(g_depth > 0);
        g_depth--;
    }

    void Reset(void)
    {
        for (uint8_t i = 0; i < kStages; i++)
        {
            g_charged[i] = 0;
        }
        g_depth = 0;
        g_stack[0] = kOther;
        g_last = Counter();
    }

    uint64_t Charged(const Stage s)
    {
        Charge();
        return g_charged[s];
    }

    const char *Name(const Stage s)
    {
        return kNames[s];
    }
} // namespace stt_prof

#endif
