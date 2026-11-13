// plugin.cpp — the speech-to-text audio plugin (stt.audioplug.elf): the
// conformer-stt-s3 engine (src/tlib, src/tlib_ops, src/conformer) fed with
// push-to-talk audio from the microphone, or with a WAV file, through the
// firmware's audio plugin ABI (src/audio/audio_plugin_abi.h).
//
// App commands (_audio_plug_write):
//   model <path>  open the model file and load its resident tensors
//   start         begin taking audio from the open mic session (key down)
//   stop          stop taking audio; what is left is transcribed (key up)
//   cancel        drop the audio not transcribed yet, end the pass running
//   file <path>   transcribe a 16 kHz mono 16-bit PCM WAV file
// While a listen or a file runs, only stop and cancel are taken; any other
// command gets "E:busy".
// Results (_audio_plug_read):
//   R             the model is loaded
//   T:<text>      one clip's text (it starts with a space when its first
//                 token does)
//   S:<line>      timing and memory figures for the step before
//   C             a listen's capture has ended (after stop or cancel): the
//                 plugin reads the microphone no more, so the app may close
//                 its session while the transcription goes on
//   D             a listen (after stop or cancel) or a file has ended
//   E:<message>   an error
//
// Clips: audio becomes 360 ms chunks; the front end (mel + pre-encode) runs
// as each chunk completes. A clip ends at the key release, at the end of a
// file, or at 11 chunks (3.96 s, plan D8): when a 12th chunk is ready, the
// clip is cut at its quietest chunk from the 7th on. That chunk ends this
// clip and starts the next one too (the overlap); this clip's text keeps
// the tokens of the overlap's first half, the next clip's text those of its
// second half, and a word that both clips (or neither) put on their side is
// matched by its token id, so a word at the cut comes out once. A clip after a cut
// whose chunks, past the overlap, are all no louder than 1.5x the overlap
// (the pause the cut found) is skipped as silence. A listen or file of
// fewer than 2 chunks is not transcribed.
//
// Timing: commands and the microphone's audio are taken after each block of
// a tiled matrix multiply (port.h stt_tile_done), after each layer and
// between chunks; a stop ends the capture when it is taken, and the audio up
// to that moment is in the clip. At the same points the worker sleeps 1 ms
// every 100 ms (Poll).

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "../../../src/audio/audio_plugin_abi.h"

#include <config.h>
#include <port.h>
#include <profile.h>
#include <tlib_heap.h>
#include <tlib_flash.h>
#include <tlib_ops.h>
#include <conformer.h>
#include <conformer_preprocessing.h>
#include <conformer_pre_encode.h>
#include <conformer_tokenizer.h>
#include <conformer_tensor_ids.h>

namespace
{
    // Internal block: the tile, then the bias rows.
    constexpr uint32_t kInternalBytes{kTileBytes + kBiasTileBytes};

    const AudioPlugHostApi *g_api = nullptr;
    uint8_t *g_internal = nullptr;
    uint8_t *g_resident = nullptr;
    uint8_t *g_resident_b = nullptr;   // the second resident block (kResidentBBytes), or nullptr
    uint8_t *g_work = nullptr;
    int16_t *g_fifo = nullptr;         // in the work block, after the heap

    // Audio FIFO (samples): the mic pump and the file reader write, the front
    // end reads. Worker task only.
    uint32_t g_fifo_head = 0;    // next sample to read
    uint32_t g_fifo_count = 0;
    uint32_t g_fifo_lost = 0;    // samples the full FIFO could not take

    int g_model = -1;                    // model file handle
    uint32_t g_model_pos = 0xFFFFFFFFu;  // its read position, unknown = all ones
    uint32_t g_read_bytes = 0;           // model bytes read since the last reset
    uint32_t g_read_ms = 0;

    enum class State : uint8_t
    {
        NoModel,
        Loading,
        Idle,
        Listening,
        File
    };

    // Written by the worker, read by stt_status() on the Lua task.
    volatile State g_state = State::NoModel;
    volatile bool g_transcribing = false;
    volatile uint32_t g_taken_samples = 0;   // audio taken in the current listen/file
    volatile uint32_t g_clip_chunks = 0;     // the clip being transcribed

    // Worker task only.
    bool g_capturing = false;      // the mic pump runs (start .. stop)
    bool g_stop_pending = false;   // stop taken: finish the rest
    bool g_cancel = false;         // cancel taken: drop the rest, end the pass
    uint32_t g_session_chunks = 0; // chunks through the front end this listen/file
    uint32_t g_mic_dropped0 = 0;   // the mic ring's drop count at start
    uint32_t g_mic_dropped = 0;    // samples the ring dropped from start to the end of capture

    char g_cmd[AUDIOPLUG_CMD_MAX + 1];        // worker loop's command
    char g_poll_cmd[AUDIOPLUG_CMD_MAX + 1];   // a command taken mid-session
    char g_text[AUDIOPLUG_RESULT_MAX + 1];    // "T:" + a clip's text
    int16_t g_scratch[256];                   // mic audio that is dropped

    constexpr uint32_t kNoToken = 0xFFFFFFFFu;

    struct Engine
    {
        conformer::Preprocessor pre;
        tlib::Tensor<float> chunks[kMaxChunks];   // pre-encoded chunks not transcribed yet
        uint32_t energy[kMaxChunks];              // mean |sample| of each
        uint8_t count = 0;
        bool overlap = false;                     // chunks[0] is the last cut's chunk
        // The last cut's chunk as the clip before it decoded it (CTC token
        // ids, kNoToken = none): its last emission in the chunk's first half
        // (kept there) and its first emission in the second half (left to
        // this clip).
        uint32_t carry_kept = kNoToken;
        uint32_t carry_next = kNoToken;
    };
    alignas(Engine) uint8_t g_engine_mem[sizeof(Engine)];
    Engine *g_engine = nullptr;

    // CTC: frame f emits its token when the token is not blank and differs
    // from frame f - 1 (a clip's frame 0 emits any non-blank token).
    bool Emits(const uint32_t *tok, const uint32_t f)
    {
        return tok[f] != conformer::kBlankToken && (f == 0 || tok[f] != tok[f - 1]);
    }

    bool IsTile(const void *p)
    {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        return b >= g_internal && b < g_internal + kInternalBytes;
    }

    void Publish(const char *s)
    {
        const uint32_t n = static_cast<uint32_t>(strlen(s));
        if (!g_api->publish(s, n > AUDIOPLUG_RESULT_MAX ? AUDIOPLUG_RESULT_MAX : n))
        {
            stt_log("result queue full, dropped: %.60s", s);
        }
    }

    void PublishF(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    void PublishF(const char *fmt, ...)
    {
        char line[200];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        Publish(line);
    }

    bool Aborted(void)
    {
        return g_cancel || g_api->should_stop();
    }

    // ── Audio FIFO ───────────────────────────────────────────────────────────

    // Drain the mic session's ring into the FIFO.
    void PumpMic(void)
    {
        if (!g_capturing)
        {
            return;
        }
        for (;;)
        {
            const uint32_t space = kAudioFifoSamples - g_fifo_count;
            if (space == 0)
            {
                const size_t n = g_api->mic_read(g_scratch, sizeof(g_scratch) / sizeof(g_scratch[0]));
                if (n == 0)
                {
                    break;
                }
                g_fifo_lost += static_cast<uint32_t>(n);
                continue;
            }
            const uint32_t tail = (g_fifo_head + g_fifo_count) % kAudioFifoSamples;
            const uint32_t run = (kAudioFifoSamples - tail < space) ? (kAudioFifoSamples - tail) : space;
            const size_t n = g_api->mic_read(g_fifo + tail, run);
            if (n == 0)
            {
                break;
            }
            g_fifo_count += static_cast<uint32_t>(n);
            g_taken_samples += static_cast<uint32_t>(n);
        }
    }

    void ClearAudio(void)
    {
        g_fifo_head = 0;
        g_fifo_count = 0;
        g_fifo_lost = 0;
        if (g_engine)
        {
            for (uint8_t i = 0; i < g_engine->count; i++)
            {
                g_engine->chunks[i] = tlib::Tensor<float>();
            }
            g_engine->count = 0;
            g_engine->overlap = false;
            g_engine->carry_kept = kNoToken;
            g_engine->carry_next = kNoToken;
        }
    }

    // One chunk from the FIFO (zero-padded when it holds less) through the
    // front end into the engine. Needs g_fifo_count > 0 and a free slot.
    void FrontEndChunk(void)
    {
        tlib::Tensor<int16_t> pcm({kChunkSize}, false, 0);
        int16_t *d = pcm.Data();
        const uint32_t take = g_fifo_count < kChunkSize ? g_fifo_count : kChunkSize;
        const uint32_t first = (kAudioFifoSamples - g_fifo_head < take) ? (kAudioFifoSamples - g_fifo_head) : take;
        memcpy(d, g_fifo + g_fifo_head, first * sizeof(int16_t));
        if (take > first)
        {
            memcpy(d + first, g_fifo, (take - first) * sizeof(int16_t));
        }
        if (take < kChunkSize)
        {
            memset(d + take, 0, (kChunkSize - take) * sizeof(int16_t));
        }
        g_fifo_head = (g_fifo_head + take) % kAudioFifoSamples;
        g_fifo_count -= take;

        uint32_t sum = 0;
        for (uint32_t i = 0; i < kChunkSize; i++)
        {
            sum += static_cast<uint32_t>(d[i] < 0 ? -d[i] : d[i]);
        }

        const uint8_t at = g_engine->count;
        g_engine->chunks[at] = conformer::PreEncode(g_engine->pre.Forward(pcm, kChunkSize));
        g_engine->energy[at] = sum / kChunkSize;
        g_engine->count++;
        g_session_chunks++;
    }

    // ── Commands taken mid-session ───────────────────────────────────────────

    // The end of a listen's capture: the ring's last audio, and its drops
    // while the capture ran (after this nobody reads the ring).
    void EndCapture(void)
    {
        PumpMic();
        g_capturing = false;
        g_mic_dropped = g_api->mic_dropped() - g_mic_dropped0;
        Publish("C");
    }

    void CmdStop(void)
    {
        if (g_state == State::Idle || g_state == State::NoModel)
        {
            Publish("D");   // nothing was listening
            return;
        }
        if (g_capturing)
        {
            EndCapture();
            g_stop_pending = true;
        }
    }

    void CmdCancel(void)
    {
        if (g_state == State::Idle || g_state == State::NoModel)
        {
            Publish("D");
            return;
        }
        if (g_capturing)
        {
            EndCapture();
        }
        g_stop_pending = false;
        g_cancel = true;
    }

    void PollCommands(void)
    {
        uint32_t n;
        while ((n = g_api->command(g_poll_cmd, AUDIOPLUG_CMD_MAX)) != 0)
        {
            g_poll_cmd[n] = 0;
            if (strcmp(g_poll_cmd, "stop") == 0)
            {
                CmdStop();
            }
            else if (strcmp(g_poll_cmd, "cancel") == 0)
            {
                CmdCancel();
            }
            else
            {
                PublishF("E:busy: %.60s", g_poll_cmd);
            }
        }
    }

    // The worker sleeps 1 ms when this long has passed since its last sleep:
    // core 1's idle task gets its turn, which the P4's task watchdog checks
    // every 5 s. A pass with the model resident (STT_MODEL_RESIDENT) waits
    // on nothing, and a clip's pass with its front end can run past 5 s.
    constexpr uint32_t kSleepEveryMs = 100;
    uint32_t g_slept_ms = 0;

    // Between layers, tiles and chunks: take the mic's audio and the app's
    // commands, and sleep 1 ms every kSleepEveryMs.
    void Poll(void)
    {
        PumpMic();
        PollCommands();
        if (g_api->ticks_ms() - g_slept_ms >= kSleepEveryMs)
        {
            g_api->delay_ms(1);
            g_slept_ms = g_api->ticks_ms();
        }
    }

    // ── Transcription ────────────────────────────────────────────────────────

    // Drop the first n chunks; the later ones move to the front. keep_last:
    // chunk n - 1 stays, as the next clip's first chunk (the overlap).
    void ShiftChunks(const uint8_t n, const bool keep_last)
    {
        const uint8_t drop = keep_last ? static_cast<uint8_t>(n - 1) : n;
        for (uint8_t i = 0; i < drop; i++)
        {
            g_engine->chunks[i] = tlib::Tensor<float>();
        }
        for (uint8_t i = drop; i < g_engine->count; i++)
        {
            g_engine->chunks[i - drop] = std::move(g_engine->chunks[i]);
            g_engine->energy[i - drop] = g_engine->energy[i];
        }
        g_engine->count = static_cast<uint8_t>(g_engine->count - drop);
        g_engine->overlap = keep_last;
    }

    // Transcribe the first n chunks as one clip. at_cut: the clip ends at a
    // cut, so its last chunk also starts the next clip (the overlap). false
    // when the pass was ended (cancel or unload).
    bool Transcribe(const uint8_t n, const bool at_cut)
    {
        const bool overlap_front = g_engine->overlap;
        if (overlap_front)
        {
            // A clip after a cut is silence when nothing past its overlap
            // is louder than 1.5x the pause the cut found (the overlap).
            uint32_t loudest = 0;
            for (uint8_t i = 1; i < n; i++)
            {
                if (g_engine->energy[i] > loudest)
                {
                    loudest = g_engine->energy[i];
                }
            }
            if (loudest * 2 <= g_engine->energy[0] * 3)
            {
                PublishF("S:skipped a quiet clip of %u ms",
                         static_cast<unsigned>(n * kChunkSize * 1000U / kSamplingRate));
                ShiftChunks(n, at_cut);
                g_engine->carry_kept = kNoToken;
                g_engine->carry_next = kNoToken;
                return true;
            }
        }

        g_clip_chunks = n;
        g_transcribing = true;
        const uint32_t t0 = g_api->ticks_ms();
        g_read_bytes = 0;
        g_read_ms = 0;
        tlib::heap::ResetMaxUsage();

        const uint32_t frames_per_chunk = g_engine->chunks[0].Shape(0);
        const tlib::Tensor<float> x = tlib::ops::cat(g_engine->chunks, n);
        ShiftChunks(n, at_cut);

        const tlib::Tensor<float> logits = conformer::Forward(x);
        g_transcribing = false;
        if (logits.Dim() == 0)
        {
            return false;   // ended mid-pass
        }
        const tlib::Tensor<uint32_t> tokens = tlib::ops::argmax(logits);
        assert(tokens.Numel() == frames_per_chunk * n);
        const uint32_t *tok = tokens.DataImm();
        // An overlap chunk's first half belongs to the clip it ends, its
        // second half to the clip it starts. The two clips see the chunk with
        // different context, so a word near its middle can fall on the same
        // side in both (twice) or on neither (lost): the token ids decide.
        const uint32_t half = frames_per_chunk / 2;
        uint32_t first = 0;
        if (overlap_front)
        {
            first = half;
            uint32_t b1_last = kNoToken, b1_frame = 0;   // last emission in [0, half)
            for (uint32_t f = 0; f < half; f++)
            {
                if (Emits(tok, f))
                {
                    b1_last = tok[f];
                    b1_frame = f;
                }
            }
            uint32_t b2_frame = frames_per_chunk;        // first emission in [half, chunk)
            for (uint32_t f = half; f < frames_per_chunk; f++)
            {
                if (Emits(tok, f))
                {
                    b2_frame = f;
                    break;
                }
            }
            if (b1_last != kNoToken && b1_last == g_engine->carry_next)
            {
                first = b1_frame;   // the clip before left this word to us: keep it
            }
            else if (b2_frame < frames_per_chunk && tok[b2_frame] == g_engine->carry_kept)
            {
                first = b2_frame;   // the clip before kept this word: skip its run
                while (first < frames_per_chunk && tok[first] == tok[b2_frame])
                {
                    first++;
                }
            }
        }
        const uint32_t end = at_cut ? frames_per_chunk * (n - 1) + half : frames_per_chunk * n;
        if (at_cut)
        {
            const uint32_t o0 = frames_per_chunk * (n - 1);   // the cut chunk's first frame
            g_engine->carry_kept = kNoToken;
            g_engine->carry_next = kNoToken;
            for (uint32_t f = o0; f < o0 + half; f++)
            {
                if (Emits(tok, f))
                {
                    g_engine->carry_kept = tok[f];
                }
            }
            for (uint32_t f = o0 + half; f < o0 + frames_per_chunk; f++)
            {
                if (Emits(tok, f))
                {
                    g_engine->carry_next = tok[f];
                    break;
                }
            }
        }
        g_text[0] = 'T';
        g_text[1] = ':';
        conformer::Decode(tokens, first, end, g_text + 2, sizeof(g_text) - 2);
        Publish(g_text);
        const uint32_t ms = g_api->ticks_ms() - t0;
        PublishF("S:clip of %u ms in %u ms; model reads %u KB in %u ms; heap peak %u KB",
                 static_cast<unsigned>(n * kChunkSize * 1000U / kSamplingRate), static_cast<unsigned>(ms),
                 static_cast<unsigned>(g_read_bytes / 1024), static_cast<unsigned>(g_read_ms),
                 static_cast<unsigned>(tlib::heap::MaxUsage() / 1024));
        stt_log("clip of %u chunks: %u ms (model reads %u KB in %u ms), heap peak %u KB of %u",
                static_cast<unsigned>(n), static_cast<unsigned>(ms), static_cast<unsigned>(g_read_bytes / 1024),
                static_cast<unsigned>(g_read_ms), static_cast<unsigned>(tlib::heap::MaxUsage() / 1024),
                static_cast<unsigned>(tlib::heap::Capacity() / 1024));
        return true;
    }

    // A clip at the cap: cut at the quietest chunk from kClipCutFrom on.
    bool TranscribeAtCap(void)
    {
        uint8_t cut = kClipMaxChunks - 1;
        for (uint8_t i = kClipCutFrom - 1; i < kClipMaxChunks; i++)
        {
            if (g_engine->energy[i] < g_engine->energy[cut])
            {
                cut = i;
            }
        }
        return Transcribe(static_cast<uint8_t>(cut + 1), true);
    }

    // The front end over every whole chunk in the FIFO; a clip at the cap is
    // transcribed once the chunk after it is ready. Returns when the FIFO
    // holds less than a chunk, or at a cancel or unload.
    void ProcessFifo(void)
    {
        for (;;)
        {
            Poll();
            if (Aborted() || g_fifo_count < kChunkSize)
            {
                return;
            }
            if (g_engine->count >= kClipMaxChunks)
            {
                TranscribeAtCap();
                continue;
            }
            FrontEndChunk();
        }
    }

    // The end of a listen or a file: the rest of the FIFO, the last partial
    // chunk, then the clip that is left.
    void Finish(void)
    {
        ProcessFifo();
        if (Aborted())
        {
            return;
        }
        if (g_fifo_count > 0)
        {
            if (g_engine->count >= kClipMaxChunks && !TranscribeAtCap())
            {
                return;
            }
            FrontEndChunk();
        }
        if (g_session_chunks >= kClipMinChunks && g_engine->count > 0)
        {
            Transcribe(g_engine->count, false);
        }
    }

    // A listen or a file is over: what was taken and lost, then D.
    void EndSession(void)
    {
        const uint32_t lost_ms = g_fifo_lost * 1000U / kSamplingRate;
        const bool was_listen = g_state == State::Listening;
        ClearAudio();
        g_capturing = false;
        g_stop_pending = false;
        g_cancel = false;
        g_state = State::Idle;
        if (was_listen)
        {
            PublishF("S:listen took %u ms of audio",
                     static_cast<unsigned>(g_taken_samples * 1000U / kSamplingRate));
        }
        if (lost_ms)
        {
            PublishF("E:%u ms of audio did not fit the buffer", static_cast<unsigned>(lost_ms));
        }
        if (was_listen && g_mic_dropped)
        {
            PublishF("E:the microphone dropped %u samples", static_cast<unsigned>(g_mic_dropped));
        }
        Publish("D");
    }

    // ── Commands taken while idle ────────────────────────────────────────────

    // The tensors the front end uses every chunk stay in memory for the session.
    const uint32_t kResidentIds[] = {
        conformer::TensorID::ENCODER_PRE_ENCODE_CONV0_WEIGHT_Q_INT8,
        conformer::TensorID::ENCODER_PRE_ENCODE_CONV0_BIAS_Q_INT32,
        conformer::TensorID::ENCODER_PRE_ENCODE_CONV1_WEIGHT_Q_INT8,
        conformer::TensorID::ENCODER_PRE_ENCODE_OUT_WEIGHT_Q_INT8,
        conformer::TensorID::MEL_FILTER_FLOAT32,
        conformer::TensorID::MEL_MEAN_FLOAT32,
        conformer::TensorID::MEL_STD_FLOAT32,
    };

    void CmdModel(const char *path)
    {
        if (g_model >= 0)
        {
            g_api->file_close(g_model);
            g_model = -1;
        }
        g_state = State::Loading;
        const uint32_t t0 = g_api->ticks_ms();
        g_model = g_api->file_open(path);
        g_model_pos = 0xFFFFFFFFu;
        if (g_model < 0)
        {
            g_state = State::NoModel;
            PublishF("E:cannot open the model file %.100s", path);
            return;
        }
        tlib::flash::FlashError err = tlib::flash::Initialize(g_resident, kResidentBytes);
        if (g_resident_b != nullptr)
        {
            tlib::flash::AddResidentRegion(g_resident_b, kResidentBBytes);
        }
#ifdef STT_MODEL_RESIDENT
        // Every tensor (config.h kResidentBytes).
        for (uint32_t id = 0; err == tlib::flash::FlashError::None && id < tlib::flash::TensorCount(); id++)
        {
            err = tlib::flash::MakeResident(id);
        }
#else
        for (uint32_t i = 0; err == tlib::flash::FlashError::None && i < sizeof(kResidentIds) / sizeof(kResidentIds[0]); i++)
        {
            err = tlib::flash::MakeResident(kResidentIds[i]);
        }
#endif
        if (err != tlib::flash::FlashError::None)
        {
            g_api->file_close(g_model);
            g_model = -1;
            g_state = State::NoModel;
            PublishF("E:model file %.100s: error %u", path, static_cast<unsigned>(err));
            return;
        }
        g_state = State::Idle;
        Publish("R");
        PublishF("S:model: %u tensors, %u KB resident, loaded in %u ms",
                 static_cast<unsigned>(tlib::flash::TensorCount()),
                 static_cast<unsigned>(tlib::flash::ResidentUsed() / 1024),
                 static_cast<unsigned>(g_api->ticks_ms() - t0));
    }

    void BeginSession(const State state)
    {
        ClearAudio();
        g_taken_samples = 0;
        g_session_chunks = 0;
        g_stop_pending = false;
        g_cancel = false;
        g_state = state;
    }

    void CmdStart(void)
    {
        if (!g_api->mic_session_open())
        {
            Publish("E:no microphone session is open");
            return;
        }
        while (g_api->mic_read(g_scratch, sizeof(g_scratch) / sizeof(g_scratch[0])))
        {
            // audio from before the key went down
        }
        BeginSession(State::Listening);
        g_mic_dropped0 = g_api->mic_dropped();
        g_mic_dropped = 0;
        g_capturing = true;
    }

    // A listen in progress: called by the worker loop until the session ends.
    void RunListen(void)
    {
        ProcessFifo();
        if (!g_cancel && g_stop_pending && !g_api->should_stop())
        {
            Finish();
        }
        if ((g_cancel || g_stop_pending) && !g_api->should_stop())
        {
            EndSession();
        }
    }

    uint32_t ReadU32(const uint8_t *p)
    {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }

#ifdef STT_PROFILE
    // The stage profile of a file (profile.h): each stage's share of the
    // file's `ms`, in ms. The sums go to 32 bits in units of 1024 cycles
    // (no 64-bit division or conversion: the host exports no libgcc).
    void PublishProfile(const uint32_t ms)
    {
        uint32_t k[stt_prof::kStages];
        float total = 0.0f;
        for (uint8_t s = 0; s < stt_prof::kStages; s++)
        {
            k[s] = static_cast<uint32_t>(stt_prof::Charged(static_cast<stt_prof::Stage>(s)) >> 10);
            total += static_cast<float>(k[s]);
        }
        char line[480];
        int at = snprintf(line, sizeof(line), "S:profile %u ms:", static_cast<unsigned>(ms));
        for (uint8_t s = 0; s < stt_prof::kStages && at < static_cast<int>(sizeof(line)); s++)
        {
            const float part = total > 0.0f ? static_cast<float>(k[s]) * static_cast<float>(ms) / total : 0.0f;
            at += snprintf(line + at, sizeof(line) - at, " %s %u%s", stt_prof::Name(static_cast<stt_prof::Stage>(s)),
                           static_cast<unsigned>(part + 0.5f), s + 1 < stt_prof::kStages ? "," : "");
        }
        Publish(line);
    }
#endif

    // Transcribe a WAV file: 16 kHz, mono, 16-bit PCM; the RIFF chunks are
    // walked to "fmt " and "data".
    void CmdFile(const char *path)
    {
        const int h = g_api->file_open(path);
        if (h < 0)
        {
            PublishF("E:cannot open %.100s", path);
            Publish("D");
            return;
        }
        uint8_t *tile = stt_tile();
        const uint32_t size = g_api->file_size(h);
        bool ok = size >= 12 && g_api->file_read(h, tile, 12) == 12 &&
                  memcmp(tile, "RIFF", 4) == 0 && memcmp(tile + 8, "WAVE", 4) == 0;
        uint32_t pos = 12, data_at = 0, data_len = 0;
        bool fmt_ok = false;
        while (ok && pos + 8 <= size && !data_at)
        {
            ok = g_api->file_seek(h, pos) && g_api->file_read(h, tile, 8) == 8;
            if (!ok)
            {
                break;
            }
            const uint32_t len = ReadU32(tile + 4);
            if (memcmp(tile, "fmt ", 4) == 0 && len >= 16)
            {
                ok = g_api->file_read(h, tile + 8, 16) == 16;
                const uint16_t format = static_cast<uint16_t>(tile[8] | (tile[9] << 8));
                const uint16_t channels = static_cast<uint16_t>(tile[10] | (tile[11] << 8));
                const uint32_t rate = ReadU32(tile + 12);
                const uint16_t bits = static_cast<uint16_t>(tile[22] | (tile[23] << 8));
                fmt_ok = format == 1 && channels == 1 && rate == kSamplingRate && bits == 16;
            }
            else if (memcmp(tile, "data", 4) == 0)
            {
                data_at = pos + 8;
                data_len = len;
            }
            pos += 8 + len + (len & 1);
        }
        if (!ok || !fmt_ok || !data_at)
        {
            g_api->file_close(h);
            PublishF("E:%.100s is not a 16 kHz mono 16-bit WAV file", path);
            Publish("D");
            return;
        }
        if (data_at + data_len > size)
        {
            data_len = size - data_at;   // a recording cut off before its header update
        }

        BeginSession(State::File);
        const uint32_t t0 = g_api->ticks_ms();
#ifdef STT_PROFILE
        stt_prof::Reset();
#endif
        uint32_t left = data_len & ~1u;
        ok = g_api->file_seek(h, data_at);
        while (ok && left && !Aborted())
        {
            // Fill the FIFO through the tile, then run the front end on it.
            uint32_t n = left < kTileBytes ? left : kTileBytes;
            const uint32_t space_bytes = (kAudioFifoSamples - g_fifo_count) * 2;
            if (n > space_bytes)
            {
                n = space_bytes;
            }
            if (n)
            {
                ok = g_api->file_read(h, tile, n) == static_cast<int32_t>(n);
                const uint32_t samples = n / 2;
                const int16_t *src = reinterpret_cast<const int16_t *>(tile);
                for (uint32_t i = 0; ok && i < samples; i++)
                {
                    g_fifo[(g_fifo_head + g_fifo_count) % kAudioFifoSamples] = src[i];
                    g_fifo_count++;
                }
                g_taken_samples += samples;
                left -= n;
            }
            ProcessFifo();
        }
        g_api->file_close(h);
        if (!ok)
        {
            PublishF("E:read error in %.100s", path);
        }
        else if (!Aborted())
        {
            Finish();
            stt_log("file %s: %u ms of audio in %u ms", path,
                    static_cast<unsigned>(g_taken_samples * 1000U / kSamplingRate),
                    static_cast<unsigned>(g_api->ticks_ms() - t0));
#ifdef STT_PROFILE
            PublishProfile(g_api->ticks_ms() - t0);
#endif
        }
        if (!g_api->should_stop())
        {
            EndSession();
        }
    }

    void HandleCommand(char *cmd)
    {
        const bool ready = g_state == State::Idle;
        if (strncmp(cmd, "model ", 6) == 0)
        {
            CmdModel(cmd + 6);
        }
        else if (strcmp(cmd, "start") == 0)
        {
            if (ready)
            {
                CmdStart();
            }
            else
            {
                Publish("E:no model loaded");
            }
        }
        else if (strcmp(cmd, "stop") == 0)
        {
            CmdStop();
        }
        else if (strcmp(cmd, "cancel") == 0)
        {
            CmdCancel();
        }
        else if (strncmp(cmd, "file ", 5) == 0)
        {
            if (ready)
            {
                CmdFile(cmd + 5);
            }
            else
            {
                Publish("E:no model loaded");
                Publish("D");
            }
        }
        else
        {
            PublishF("E:unknown command: %.60s", cmd);
        }
    }

    // ── Plugin entry points ──────────────────────────────────────────────────

    bool stt_init(const AudioPlugHostApi *api, void *const *blocks)
    {
        g_api = api;
        g_internal = static_cast<uint8_t *>(blocks[0]);
        g_resident = static_cast<uint8_t *>(blocks[1]);
        g_resident_b = static_cast<uint8_t *>(blocks[2]);
        g_work = static_cast<uint8_t *>(blocks[3]);
        g_fifo = reinterpret_cast<int16_t *>(g_work + kWorkHeapBytes);
        tlib::heap::Initialize(g_work, kWorkHeapBytes);
        g_state = State::NoModel;
        return true;
    }

    void stt_worker(const AudioPlugHostApi *api)
    {
        g_engine = new (g_engine_mem) Engine();
        while (!api->should_stop())
        {
            if (g_state == State::Listening)
            {
                RunListen();
                if (g_state == State::Listening)
                {
                    api->wait(50);
                }
                continue;
            }
            api->wait(200);
            uint32_t n;
            while (g_state != State::Listening && !api->should_stop() &&
                   (n = api->command(g_cmd, AUDIOPLUG_CMD_MAX)) != 0)
            {
                g_cmd[n] = 0;
                HandleCommand(g_cmd);
            }
        }
        g_capturing = false;
        ClearAudio();
        g_engine->~Engine();
        g_engine = nullptr;
    }

    void stt_status(char *out, uint32_t n)
    {
        if (g_transcribing)
        {
            const uint32_t tenths = g_clip_chunks * 36 / 10;   // 360 ms chunks
            snprintf(out, n, "transcribing %u.%u s", static_cast<unsigned>(tenths / 10),
                     static_cast<unsigned>(tenths % 10));
            return;
        }
        switch (g_state)
        {
        case State::NoModel:
            snprintf(out, n, "no model");
            break;
        case State::Loading:
            snprintf(out, n, "loading the model");
            break;
        case State::Idle:
            snprintf(out, n, "ready");
            break;
        case State::Listening:
            snprintf(out, n, "listening %u.%u s", static_cast<unsigned>(g_taken_samples / kSamplingRate),
                     static_cast<unsigned>((g_taken_samples % kSamplingRate) / (kSamplingRate / 10)));
            break;
        case State::File:
            snprintf(out, n, "reading the file, %u s", static_cast<unsigned>(g_taken_samples / kSamplingRate));
            break;
        }
    }

    void stt_deinit(const AudioPlugHostApi *api)
    {
        if (g_model >= 0)
        {
            api->file_close(g_model);
            g_model = -1;
        }
        g_api = nullptr;
    }
} // namespace

// ── Engine hooks (port.h) ────────────────────────────────────────────────────

extern "C" void stt_log(const char *fmt, ...)
{
    if (!g_api)
    {
        return;
    }
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_api->log("%s", line);
}

// A failed assert (newlib's <assert.h> calls this): logged, then the trap
// stops the device with the plugin's backtrace.
extern "C" void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    stt_log("FAIL: assert %s at %s:%d (%s)", expr, file, line, func ? func : "?");
    __builtin_trap();
}

bool stt_model_read(uint32_t offset, void *dst, uint32_t len)
{
    STT_PROF(kRead);
    if (g_model < 0)
    {
        stt_log("FAIL: model read with no model open");
        return false;
    }
    const uint32_t t0 = g_api->ticks_ms();
    if (offset != g_model_pos)
    {
        if (!g_api->file_seek(g_model, offset))
        {
            g_model_pos = 0xFFFFFFFFu;
            stt_log("FAIL: model seek to %u", static_cast<unsigned>(offset));
            return false;
        }
        g_model_pos = offset;
    }
    uint8_t *out = static_cast<uint8_t *>(dst);
    const bool direct = IsTile(dst);
    uint32_t done = 0;
    while (done < len)
    {
        uint32_t n = len - done;
        uint8_t *target = out + done;
        if (!direct)
        {
            if (n > kTileBytes)
            {
                n = kTileBytes;
            }
            target = stt_tile();
        }
        const int32_t got = g_api->file_read(g_model, target, n);
        if (got != static_cast<int32_t>(n))
        {
            g_model_pos = 0xFFFFFFFFu;
            stt_log("FAIL: model read of %u bytes at %u returned %d", static_cast<unsigned>(n),
                    static_cast<unsigned>(offset + done), static_cast<int>(got));
            return false;
        }
        if (!direct)
        {
            memcpy(out + done, target, n);
        }
        done += n;
    }
    g_model_pos = offset + len;
    g_read_bytes += len;
    g_read_ms += g_api->ticks_ms() - t0;
    return true;
}

uint8_t *stt_tile(void) { return g_internal; }

int32_t *stt_bias_tile(void) { return reinterpret_cast<int32_t *>(g_internal + kTileBytes); }

void stt_tile_done(void) { Poll(); }

bool stt_layer_done(uint16_t layer)
{
    (void)layer;
    Poll();
    return Aborted();
}

// The module's one export (build.ps1 hides every other symbol).
extern "C" __attribute__((visibility("default"))) const AudioPlugDesc audioplug_ops = {
    AUDIO_PLUGIN_ABI_VERSION,
    "stt",
    { { { kInternalBytes,                                       AUDIOPLUG_MEM_INTERNAL },
        { kResidentBytes,                                       AUDIOPLUG_MEM_PSRAM },
        { kResidentBBytes,                                      kResidentBBytes ? AUDIOPLUG_MEM_PSRAM : AUDIOPLUG_MEM_NONE },
        { kWorkHeapBytes + kAudioFifoSamples * sizeof(int16_t), AUDIOPLUG_MEM_PSRAM } },
      8 * 1024 },
    stt_init,
    stt_worker,
    stt_status,
    stt_deinit,
};
