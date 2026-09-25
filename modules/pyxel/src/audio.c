// Four-channel sound synthesis. Playback semantics follow pyxel-core
// channel.rs, voice.rs, sound.rs and tone.rs (MIT, Takashi Kitao): notes of
// `speed` ticks at 120 ticks/s, wavetable/noise tones, volume 0-7, slide,
// vibrato and fade-out effects, loop and resume.
//
// Samples are synthesized on the module's own core and pushed to the
// firmware's audio ring, kept PX_AUDIO_LEAD samples ahead of real time.
// px_audio_poll() runs from the MicroPython VM hook, so the ring stays fed
// while Python code runs.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

extern void host_audio_push(const int16_t* samples, int count, int sample_rate);

#define RATE            22050
#define Q_PER_SAMPLE    4                    // time unit: quarter samples
#define Q_PER_TICK      735                  // RATE * 4 / 120
#define LEAD            1654                 // 75 ms ahead of real time
#define MIN_BATCH       256
#define CHUNK           512
#define RAMP_SAMPLES    18                   // 0.8 ms gain interpolation
#define CONTROL_SAMPLES 32                   // pitch/envelope update interval
#define POLL_US         4000

enum { FX_NONE, FX_SLIDE, FX_VIBRATO, FX_FADEOUT, FX_HALF_FADEOUT, FX_QUARTER_FADEOUT };
enum { MODE_WAVETABLE, MODE_SHORT_NOISE, MODE_LONG_NOISE };

typedef struct {
    int mode;
    int len;
    float wave[PX_TONE_MAX_SAMPLES];
    float gain;
} tone_t;

typedef struct {
    // sequence
    px_snd* snds;
    int nsnd;
    bool loop;
    px_snd* rsnds;
    int nrsnd;
    bool rloop;
    bool resume;
    bool playing;
    int si, ni;                   // sound index, next note index
    int64_t note_len_q, note_pos_q;
    int64_t sound_elapsed_q, total_elapsed_q, playback_q;
    float gain, detune;
    // current note
    bool note_on;
    float midi, vol, glide_off, last_midi;
    bool has_last;
    int tone, fx, speed;
    // voice
    float freq;                   // oscillator steps per sample
    float env;
    int control_left;
    float phase;
    int wave_idx;
    uint16_t lfsr;
    int lfsr_tap;
    float cur_gain, ramp_from;
    int ramp_left;
} chan_t;

static tone_t s_tones[PX_NUM_TONES];
static chan_t s_ch[PX_NUM_CHANNELS];
static bool s_enabled;
static bool s_started;
static int64_t s_pushed;          // samples handed to the ring
static int64_t s_elapsed_us;      // real time since the first push
static uint32_t s_last_us;
static uint32_t s_last_poll_us;
static int16_t s_buf[CHUNK];

uint32_t px_audio_samples, px_audio_underruns, px_audio_us;

// ---------------------------------------------------------------------------
// Tones
// ---------------------------------------------------------------------------
void px_audio_set_tone(int i, int mode, const uint32_t* table, int len, int sample_bits, float gain) {
    if (i < 0 || i >= PX_NUM_TONES) return;
    tone_t* t = &s_tones[i];
    t->mode = mode;
    t->gain = gain;
    if (len > PX_TONE_MAX_SAMPLES) len = PX_TONE_MAX_SAMPLES;
    t->len = 0;
    if (sample_bits < 1 || sample_bits > 16) return;
    float max = (float)((1u << sample_bits) - 1);
    for (int k = 0; k < len; k++) {
        uint32_t v = table[k] > (uint32_t)max ? (uint32_t)max : table[k];
        t->wave[k] = (float)v / max * 2.0f - 1.0f;
    }
    t->len = len;
}

// ---------------------------------------------------------------------------
// Sequencing
// ---------------------------------------------------------------------------
static void free_snds(px_snd* s, int n) {
    if (!s) return;
    for (int i = 0; i < n; i++) free(s[i].ev);
    free(s);
}

static void start_ramp(chan_t* c) {
    c->ramp_from = c->cur_gain;
    c->ramp_left = RAMP_SAMPLES;
}

static void set_noise(chan_t* c, bool short_period) {
    int tap = short_period ? 6 : 1;
    if (c->lfsr_tap != tap) {
        c->lfsr_tap = tap;
        c->lfsr = short_period ? 0x0201 : 0x7001;
    }
}

static void update_control(chan_t* c) {
    const tone_t* t = &s_tones[c->tone];
    float ticks = (float)c->note_pos_q / Q_PER_TICK;
    float semis = 0.0f;
    if (c->fx == FX_VIBRATO) {
        // 6 Hz triangle, +-25 cents, phase follows playback time
        float phase = (float)c->playback_q / Q_PER_TICK / 20.0f + 0.25f;
        phase -= floorf(phase);
        semis += (1.0f - 4.0f * fabsf(phase - 0.5f)) * 0.25f;
    } else if (c->fx == FX_SLIDE && ticks < (float)c->speed) {
        semis += c->glide_off - c->glide_off * ticks / (float)c->speed;
    }
    float hz = 440.0f * exp2f((c->midi + semis - 69.0f) / 12.0f);
    int spc = (t->mode == MODE_WAVETABLE) ? (t->len > 0 ? t->len : 1) : 1;
    c->freq = hz * (float)spc / (float)RATE;

    float env = 1.0f;
    float sp = (float)c->speed;
    if (c->fx == FX_FADEOUT) {
        env = 1.0f - ticks / sp;
    } else if (c->fx == FX_HALF_FADEOUT || c->fx == FX_QUARTER_FADEOUT) {
        float fade = roundf(sp / (c->fx == FX_HALF_FADEOUT ? 2.0f : 4.0f));
        float hold = sp - fade;
        env = ticks < hold ? 1.0f : (fade > 0.0f ? 1.0f - (ticks - hold) / fade : 0.0f);
    }
    c->env = env < 0.0f ? 0.0f : env;
    c->control_left = CONTROL_SAMPLES;
}

// Starts the next note or rest of the current sound, moving to the next
// sound (loop / resume / stop) at the end of one. `quiet` skips voice setup
// (seeking).
static void play_from(chan_t* c, int64_t start_q);

static void next_event(chan_t* c, bool quiet) {
    for (int guard = 0; guard < 4096 && c->playing; guard++) {
        px_snd* s = &c->snds[c->si];
        if (c->ni < s->n) {
            const px_note_ev* e = &s->ev[c->ni++];
            c->speed = s->speed;
            c->note_len_q = (int64_t)s->speed * Q_PER_TICK;
            c->note_pos_q = 0;
            if (e->note < 0) {
                if (c->note_on && !quiet) start_ramp(c);
                c->note_on = false;
                return;
            }
            int tone = e->tone < PX_NUM_TONES ? e->tone : 0;
            const tone_t* t = &s_tones[tone];
            float midi = (float)e->note + (t->mode == MODE_WAVETABLE ? 36.0f : 60.0f) + c->detune / 100.0f;
            c->fx = e->fx;
            c->glide_off = (c->fx == FX_SLIDE && c->has_last) ? c->last_midi - midi : 0.0f;
            c->last_midi = midi;
            c->has_last = true;
            c->midi = midi;
            c->tone = tone;
            c->vol = (float)e->vol / 7.0f;
            if (t->mode != MODE_WAVETABLE) set_noise(c, t->mode == MODE_SHORT_NOISE);
            else c->lfsr_tap = 0;
            if (c->wave_idx >= t->len) c->wave_idx = 0;
            c->note_on = true;
            if (!quiet) {
                start_ramp(c);
                update_control(c);
            }
            return;
        }
        // end of sound
        c->si++;
        c->ni = 0;
        c->sound_elapsed_q = 0;
        if (c->si < c->nsnd) continue;
        if (c->loop) {
            c->si = 0;
            continue;
        }
        if (c->resume) {
            px_snd* rs = c->rsnds;
            int rn = c->nrsnd;
            bool rl = c->rloop;
            c->rsnds = NULL;
            c->nrsnd = 0;
            free_snds(c->snds, c->nsnd);
            c->snds = rs;
            c->nsnd = rn;
            c->loop = rl;
            c->resume = false;
            play_from(c, c->total_elapsed_q);
            return;
        }
        c->playing = false;
    }
    if (c->note_on && !quiet) start_ramp(c);
    c->note_on = false;
    c->playing = false;
}

// Advance time by q quarter-samples, crossing note boundaries.
static void advance(chan_t* c, int64_t q, bool quiet) {
    while (q > 0 && c->playing) {
        int64_t left = c->note_len_q - c->note_pos_q;
        if (left <= 0) {
            next_event(c, quiet);
            continue;
        }
        int64_t step = q < left ? q : left;
        c->note_pos_q += step;
        c->sound_elapsed_q += step;
        c->total_elapsed_q += step;
        c->playback_q += step;
        q -= step;
        if (c->note_pos_q >= c->note_len_q) next_event(c, quiet);
    }
}

static int64_t loop_length_q(const chan_t* c) {
    int64_t total = 0;
    for (int i = 0; i < c->nsnd; i++) total += (int64_t)c->snds[i].n * c->snds[i].speed * Q_PER_TICK;
    return total;
}

static void play_from(chan_t* c, int64_t start_q) {
    bool any = false;
    for (int i = 0; i < c->nsnd; i++) any |= c->snds[i].n > 0;
    c->total_elapsed_q = 0;
    c->playing = any;
    c->si = 0;
    c->ni = 0;
    c->note_len_q = c->note_pos_q = 0;
    c->sound_elapsed_q = 0;
    c->playback_q = 0;
    c->has_last = false;
    if (!any) {
        if (c->note_on) start_ramp(c);
        c->note_on = false;
        return;
    }
    if (start_q > 0) {
        int64_t seek = start_q;
        if (c->loop) {
            int64_t len = loop_length_q(c);
            if (len > 0) seek %= len;
        }
        advance(c, seek, true);
        if (seek != start_q) c->total_elapsed_q = start_q;
        if (c->playing && c->note_on) update_control(c);
    }
    if (c->note_pos_q >= c->note_len_q) next_event(c, false);
    start_ramp(c);
}

void px_audio_play(int ch, px_snd* snds, int n, float sec, bool loop, bool resume,
                   float gain, float detune) {
    if (ch < 0 || ch >= PX_NUM_CHANNELS) { free_snds(snds, n); return; }
    chan_t* c = &s_ch[ch];
    int64_t keep_total = c->total_elapsed_q;
    if (resume && !c->resume) {
        free_snds(c->rsnds, c->nrsnd);
        c->rsnds = c->snds;
        c->nrsnd = c->nsnd;
        c->rloop = c->loop;
        c->snds = NULL;
        c->nsnd = 0;
    }
    free_snds(c->snds, c->nsnd);
    c->snds = snds;
    c->nsnd = n;
    c->loop = loop;
    c->resume = resume;
    c->gain = gain;
    c->detune = detune;
    play_from(c, (int64_t)(sec * (float)RATE + 0.5f) * Q_PER_SAMPLE);
    if (resume) c->total_elapsed_q = keep_total + c->total_elapsed_q;
}

void px_audio_stop(int ch) {
    for (int i = 0; i < PX_NUM_CHANNELS; i++) {
        if (ch >= 0 && i != ch) continue;
        chan_t* c = &s_ch[i];
        c->playing = false;
        if (c->note_on) start_ramp(c);
        c->note_on = false;
        c->resume = false;
        free_snds(c->rsnds, c->nrsnd);
        c->rsnds = NULL;
        c->nrsnd = 0;
    }
}

bool px_audio_pos(int ch, int* sound_index, float* sec) {
    if (ch < 0 || ch >= PX_NUM_CHANNELS || !s_ch[ch].playing) return false;
    *sound_index = s_ch[ch].si;
    *sec = (float)s_ch[ch].sound_elapsed_q / (float)(RATE * Q_PER_SAMPLE);
    return true;
}

// ---------------------------------------------------------------------------
// Synthesis
// ---------------------------------------------------------------------------
static inline float osc_value(const chan_t* c) {
    const tone_t* t = &s_tones[c->tone];
    if (t->mode == MODE_WAVETABLE) return c->wave_idx < t->len ? t->wave[c->wave_idx] : 0.0f;
    return (c->lfsr & 1) ? -1.0f : 1.0f;
}

static inline void osc_step(chan_t* c) {
    const tone_t* t = &s_tones[c->tone];
    if (t->mode == MODE_WAVETABLE) {
        if (++c->wave_idx >= t->len) c->wave_idx = 0;
    } else {
        uint16_t fb = (uint16_t)((c->lfsr ^ (c->lfsr >> c->lfsr_tap)) & 1);
        c->lfsr = (uint16_t)(((c->lfsr >> 1) | (fb << 14)) & 0x7FFF);
    }
}

static bool chan_audible(const chan_t* c) {
    return c->note_on || c->ramp_left > 0 || c->cur_gain != 0.0f;
}

// One output sample of channel c (box-filtered step waveform), then advance.
static float chan_sample(chan_t* c) {
    float out = 0.0f;
    if (chan_audible(c)) {
        if (c->note_on && --c->control_left <= 0) update_control(c);
        float target = c->note_on ? c->gain * c->vol * s_tones[c->tone].gain * c->env : 0.0f;
        float g = target;
        if (c->ramp_left > 0) {
            g = c->ramp_from + (target - c->ramp_from) * (1.0f - (float)c->ramp_left / RAMP_SAMPLES);
            c->ramp_left--;
        }
        c->cur_gain = g;
        if (g != 0.0f) {
            float rem = c->freq, acc = 0.0f, v = osc_value(c);
            if (rem > 0.0f) {
                while (c->phase + rem >= 1.0f) {
                    float part = 1.0f - c->phase;
                    acc += v * part;
                    rem -= part;
                    c->phase = 0.0f;
                    osc_step(c);
                    v = osc_value(c);
                }
                acc += v * rem;
                c->phase += rem;
                out = acc / c->freq * g;
            }
        } else if (!c->note_on) {
            c->cur_gain = 0.0f;
        }
    }
    if (c->playing) advance(c, Q_PER_SAMPLE, false);
    return out;
}

static bool any_audible(void) {
    for (int i = 0; i < PX_NUM_CHANNELS; i++)
        if (s_ch[i].playing || chan_audible(&s_ch[i])) return true;
    return false;
}

// One-pole DC blocker, pole 1 - 1/512: the high-pass blip_buf applies to
// upstream's output (bass_shift 9, about 7 Hz at 22050 Hz).
static float s_dc_x, s_dc_y;

static void synth(int16_t* out, int n) {
    for (int k = 0; k < n; k++) {
        float mix = 0.0f;
        for (int i = 0; i < PX_NUM_CHANNELS; i++) mix += chan_sample(&s_ch[i]);
        s_dc_y = mix - s_dc_x + s_dc_y * (1.0f - 1.0f / 512.0f);
        s_dc_x = mix;
        float f = s_dc_y * 32767.0f;
        int v = (int)(f + (f >= 0.0f ? 0.5f : -0.5f));
        out[k] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
}

void px_audio_service(void) {
    uint32_t now = host_get_ticks_us();
    s_elapsed_us += (uint32_t)(now - s_last_us);
    s_last_us = now;
    s_last_poll_us = now;
    int64_t consumed = s_elapsed_us * RATE / 1000000;
    int64_t fill = s_pushed - consumed;
    bool audible = any_audible();

    if (!s_enabled) {
        // Keep the sequencer on time without producing sound.
        if (fill < 0) {
            for (int i = 0; i < PX_NUM_CHANNELS; i++)
                if (s_ch[i].playing) advance(&s_ch[i], -fill * Q_PER_SAMPLE, true);
            s_pushed = consumed;
        }
        return;
    }
    if (fill < 0) {
        if (audible && s_started) px_audio_underruns++;
        s_pushed = consumed;
        fill = 0;
    }
    if (!audible) {
        s_started = false;
        return;
    }
    int64_t want = LEAD - fill;
    if (want < MIN_BATCH && s_started) return;
    s_started = true;
    uint32_t t0 = host_get_ticks_us();
    while (want > 0) {
        int n = want < CHUNK ? (int)want : CHUNK;
        synth(s_buf, n);
        host_audio_push(s_buf, n, RATE);
        s_pushed += n;
        px_audio_samples += (uint32_t)n;
        want -= n;
    }
    px_audio_us += host_get_ticks_us() - t0;
}

void px_audio_poll(void) {
    uint32_t now = host_get_ticks_us();
    if ((uint32_t)(now - s_last_poll_us) < POLL_US) return;
    px_audio_service();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
static const uint32_t TRIANGLE[32] = {
    8, 9, 10, 11, 12, 13, 14, 15, 15, 14, 13, 12, 11, 10, 9, 8,
    7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7,
};
static const uint32_t SQUARE[2] = { 1, 0 };
static const uint32_t PULSE[4] = { 1, 0, 0, 0 };

void px_audio_init(bool enabled) {
    px_audio_shutdown();
    s_enabled = enabled;
    px_audio_set_tone(0, MODE_WAVETABLE, TRIANGLE, 32, 4, 1.0f);
    px_audio_set_tone(1, MODE_WAVETABLE, SQUARE, 2, 1, 0.3f);
    px_audio_set_tone(2, MODE_WAVETABLE, PULSE, 4, 1, 0.3f);
    px_audio_set_tone(3, MODE_LONG_NOISE, NULL, 0, 4, 0.6f);
    s_last_us = host_get_ticks_us();
    s_last_poll_us = s_last_us;
}

void px_audio_shutdown(void) {
    for (int i = 0; i < PX_NUM_CHANNELS; i++) {
        free_snds(s_ch[i].snds, s_ch[i].nsnd);
        free_snds(s_ch[i].rsnds, s_ch[i].nrsnd);
    }
    memset(s_ch, 0, sizeof(s_ch));
    s_dc_x = s_dc_y = 0.0f;
    s_pushed = 0;
    s_elapsed_us = 0;
    s_started = false;
}
