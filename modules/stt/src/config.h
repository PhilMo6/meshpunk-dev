// config.h — the engine's configuration. The audio and model-file values are
// conformer-stt-s3's (libs/config/inc_esp32s3/config.h, Apache-2.0); the
// memory values are the Meshpunk plugin's (modules/stt/src/plugin.cpp).

#ifndef CONFIG_H_
#define CONFIG_H_

#include <cstddef>
#include <cstdint>

// Audio: 16 kHz mono; the model is trained on 16 kHz audio only.
inline constexpr const uint32_t kSamplingRate{16'000U};
// Samples per chunk (360 ms); the front end turns one chunk into 8 frames.
inline constexpr const uint32_t kChunkSize{160 * 36};
// Size of the fourier transform. kNFFT/2 must be a power of 4 (radix-4 FFT).
inline constexpr const uint16_t kNFFT{512};
// Window length for the input of the fourier transform.
inline constexpr const uint16_t kWinLength{512};
// Hop length for the short-time fourier transform.
inline constexpr const uint16_t kHopLength{160};

// Model file: max tensor dimensions, tensor data alignment, meta partition.
inline constexpr const uint8_t kFlashTensorMaxDims{4};
inline constexpr const uint8_t kFlashTensorAlignment{16};
inline constexpr const uint32_t kFlashMetaPartitionSize{64 * 1'024};

// Tensor heap: byte alignment and live blocks. One PSRAM heap holds what
// upstream split between an SRAM heap and a PSRAM heap (64 blocks each).
inline constexpr const uint8_t kHeapAlignment{16};
inline constexpr const uint16_t kHeapMaxBlocks{128};

// The internal-SRAM tile the S3 kernels re-read their B operand from, and
// the bias rows that go with it (int32 per output feature; 256 rows: the
// most a tile holds of any biased layer is 93). The portable kernels take B
// in blocks of kTileBytes where it is, the tile only for B read from SD.
inline constexpr const uint32_t kTileBytes{16 * 1'024};
inline constexpr const uint32_t kBiasTileBytes{1 * 1'024};

// Plugin memory (plugin.cpp needs): the tensor heap for activations, the
// blocks the model's resident tensors and tensor table live in, and the
// audio FIFO between the microphone and the front end. STT_MODEL_RESIDENT
// (the P4 build: 32 MB PSRAM) keeps the whole model there (13,735,424
// bytes with the table), so no clip reads the SD card, in two blocks of
// 7 MiB (944 KB to spare): a load needs no 14 MB of contiguous PSRAM.
// Elsewhere one block holds the front end's tensors only.
inline constexpr const uint32_t kWorkHeapBytes{1'536 * 1'024};
#ifdef STT_MODEL_RESIDENT
inline constexpr const uint32_t kResidentBytes{7 * 1'024 * 1'024};
inline constexpr const uint32_t kResidentBBytes{7 * 1'024 * 1'024};
#else
inline constexpr const uint32_t kResidentBytes{1'024 * 1'024};
inline constexpr const uint32_t kResidentBBytes{0};
#endif
inline constexpr const uint32_t kAudioFifoSamples{6 * kSamplingRate};

// Clips: the cap (11 chunks = 3.96 s; plan D8) and where a cut may fall
// when the cap is reached (the quietest chunk from the 7th on).
inline constexpr const uint8_t kClipMaxChunks{11};
inline constexpr const uint8_t kClipCutFrom{7};
// Fewer chunks than this at the end of a clip are not transcribed.
inline constexpr const uint8_t kClipMinChunks{2};
// Pre-encoded chunks kept at once (a clip plus the FIFO's worth).
inline constexpr const uint8_t kMaxChunks{32};

#endif
