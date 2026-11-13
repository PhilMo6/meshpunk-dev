// profile.h — the engine's stage profile, in the builds with STT_PROFILE
// (build.ps1 -Variant rv32prof): the CPU's cycle counter is charged to the
// stage running; a stage entered inside another pauses the outer one until
// it leaves, so the stages add up to the time profiled. plugin.cpp resets
// the sums when a file starts and publishes them when it ends. Without
// STT_PROFILE, STT_PROF() expands to nothing.

#ifndef STT_PROFILE_H_
#define STT_PROFILE_H_

#include <cstdint>

namespace stt_prof
{
    enum Stage : uint8_t
    {
        kOther,        // in none of the stages below
        kSums,         // int8 sums (the kernels' row dots)
        kInt8Post,     // int8 kernels outside their sums: bias, shift, clamp, scale
        kF32,          // float matmul (bmm)
        kQuantize,
        kTileCopy,     // weight rows into the tile, output blocks into place
        kRead,         // model file reads
        kSoftmax,
        kNorm,         // layer norm, batch norm
        kGlu,
        kDwConv,       // depthwise convolution outside its sums (unfold, calls)
        kConv2d,       // pre-encode convolution outside its sums
        kTranspose,
        kElementwise,  // add, scale, relu, cat, argmax
        kAttnMisc,     // attention: bias add, relative shift, slice
        kMel,          // preprocessing: window, FFT, mel, log, normalize
        kStages
    };

#ifdef STT_PROFILE
    void Enter(Stage s);
    void Leave(void);
    void Reset(void);
    // Cycles charged to stage s since the last Reset().
    uint64_t Charged(Stage s);
    const char *Name(Stage s);

    struct Scope
    {
        explicit Scope(const Stage s) { Enter(s); }
        ~Scope() { Leave(); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };
#endif
} // namespace stt_prof

#ifdef STT_PROFILE
#define STT_PROF_CAT2(a, b) a##b
#define STT_PROF_CAT(a, b) STT_PROF_CAT2(a, b)
#define STT_PROF(stage) const stt_prof::Scope STT_PROF_CAT(stt_prof_scope_, __LINE__)(stt_prof::stage)
#else
#define STT_PROF(stage)
#endif

#endif
