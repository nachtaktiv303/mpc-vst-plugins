// DUB STATION - a dub-techno tape delay + huge (Airwindows Galactic) reverb for MPC OS.
//
// DSP: the CHOMPI "TAPE 2.0" interpolated tape delay, warble and DJ filter (src/chompi/, MIT, see
// src/VENDORED.md), Airwindows Galactic for the reverb (src/airwindows/galactic.h, MIT), plus degrade,
// tape noise and ducking written here. Exposed through mpc-vst-plugins' engine interface (wrapper/engine.h).
//
// Signal path (per channel): the dry input is kept; the wet path is
//   in -> drive (tape sat) -> warble -> tape delay (filtered + degraded, ping-pong, self-oscillating
//         feedback) -> + noise -> Galactic reverb -> ducking ; then mix(dry, wet) -> out
//
// 44.1 kHz / 128-frame blocks, 16-bit stereo in/out.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "daisysp/dsp.h"
#include "daisysp/delayline.h"
#include "chompi/InterpolatedDelayLine.h"
#include "chompi/Warble.h"
#include "chompi/DJFilter.h"
#include "airwindows/galactic.h"

extern "C" {
typedef struct
{
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
    void (*process)(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames);
} mpc_engine_t;
const mpc_engine_t *mpc_engine(void);
}

using namespace daisysp;

namespace
{
const float  kSampleRate   = 44100.f;
const size_t kMaxDelayTime = 88435;
const float  kMinDelayTime = 413.f;

// note divisions the TIME knob snaps to in Sync mode (shortest .. longest)
const float       kDivBeats[7]  = {0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 4.f};
const char *const kDivLabels[7] = {"1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1"};

inline float s162f(int32_t x) { return (float)x * 3.0518509475997192e-05f; }
inline int32_t f2s16(float x)
{
    x = x <= -0.999985f ? -0.999985f : x;
    x = x >= 0.999985f ? 0.999985f : x;
    return (int32_t)(x * 32767.0f);
}

// Stereo width on a ping-pong signal (w in [0,1]): mid/side scaling. w=1 keeps the full ping-pong,
// w=0 collapses to mono (centre). Narrows/widens the image without losing the internal cross-feedback.
inline void WidthStereo(float &l, float &r, float w)
{
    const float mid  = 0.5f * (l + r);
    const float side = 0.5f * (l - r) * w;
    l = mid + side;
    r = mid - side;
}

// A short delay tap (circular buffer) for a tiny inter-channel Haas offset that grows with width.
const int kHaasMax = 160;
inline float HaasTap(float *buf, int &idx, float x, int off)
{
    buf[idx] = x;
    if(off < 0)
        off = 0;
    else if(off > kHaasMax - 1)
        off = kHaasMax - 1;
    int r = idx - off;
    if(r < 0)
        r += kHaasMax;
    const float out = buf[r];
    if(++idx >= kHaasMax)
        idx = 0;
    return out;
}

struct DcBlock
{
    float in_, out_;
    void  Init() { in_ = out_ = 0.f; }
    float Process(float in)
    {
        const float out = in - in_ + 0.999f * out_;
        out_            = out;
        in_             = in;
        return out;
    }
};

struct Degrade
{
    float holdl_, holdr_, phase_, lpl_, lpr_;
    void  Init() { holdl_ = holdr_ = phase_ = lpl_ = lpr_ = 0.f; }
    // ratio = sample&hold rate (SR reduction), q = quantization step (bit depth),
    // lpc = reconstruction LP coeff that tracks the decimation rate
    void Process(float ratio, float q, float lpc, float &l, float &r)
    {
        phase_ += ratio;
        if(phase_ >= 1.f)
        {
            phase_ -= 1.f;
            holdl_ = roundf(l * q) / q; // quantize at the capture instant, like a sampler
            holdr_ = roundf(r * q) / q;
        }
        // reconstruction LP smooths the zero-order-hold steps -> rolls off the aliasing images,
        // so it reads as warm vintage-sampler grit instead of harsh digital hash
        lpl_ += lpc * (holdl_ - lpl_);
        lpr_ += lpc * (holdr_ - lpr_);
        l = lpl_;
        r = lpr_;
    }
};

// 4-stage modulated all-pass phaser with resonance feedback, stereo (R LFO quarter-phase offset).
struct Phaser
{
    static const int kStages = 4;
    float            ap_[2][kStages];
    float            fb_[2];   // feedback state (sharpens the notches = resonance)
    float            lfo_;
    void             Init()
    {
        for(int c = 0; c < 2; c++)
        {
            for(int s = 0; s < kStages; s++)
                ap_[c][s] = 0.f;
            fb_[c] = 0.f;
        }
        lfo_ = 0.f;
    }
    float Stage(int ch, float in, float g)
    {
        float x = in;
        for(int s = 0; s < kStages; s++)
        {
            const float y = -g * x + ap_[ch][s];
            ap_[ch][s]    = x + g * y;
            x             = y;
        }
        return x;
    }
    // advance the LFO once per sample, then phase L and R; mix 0..1 blends dry<->phased up to full wet.
    void Process(float &l, float &r, float rate_hz, float mix)
    {
        lfo_ += rate_hz * (1.f / 44100.f);
        if(lfo_ >= 1.f)
            lfo_ -= 1.f;
        const float ml = 0.5f * (1.f - cosf(6.2831853f * lfo_));
        float       pr = lfo_ + 0.25f;
        if(pr >= 1.f)
            pr -= 1.f;
        const float mr = 0.5f * (1.f - cosf(6.2831853f * pr));
        const float gl = 0.15f + 0.82f * ml;  // wider all-pass coeff sweep 0.15 .. 0.97
        const float gr = 0.15f + 0.82f * mr;
        const float kFb = 0.6f;                // feedback deepens/sharpens the notches
        const float pl  = Stage(0, l + fb_[0] * kFb, gl);
        const float prr = Stage(1, r + fb_[1] * kFb, gr);
        fb_[0]          = pl;
        fb_[1]          = prr;
        // full wet at mix=1 (pure all-pass chain = deepest notches); linear dry<->wet blend
        l = l * (1.f - mix) + pl * mix;
        r = r * (1.f - mix) + prr * mix;
    }
};

// Flanger on Delay B: a short LFO-swept delay with light feedback, mixed with the input. One knob (0..1)
// scales the sweep depth, the wet mix and the feedback together; a slow fixed LFO gives the jet sweep.
struct Flanger
{
    static const int kBuf = 1024; // ~23 ms; the sweep uses ~0.5..7.5 ms
    float            bufL[kBuf], bufR[kBuf];
    int              w_;
    float            lfo_, fbL_, fbR_;
    void             Init()
    {
        for(int i = 0; i < kBuf; i++)
            bufL[i] = bufR[i] = 0.f;
        w_   = 0;
        lfo_ = 0.f;
        fbL_ = fbR_ = 0.f;
    }
    static float Read(const float *buf, float rp)
    {
        rp += (float)kBuf; // keep positive -> no floorf needed
        const int   ri = (int)rp;
        const int   i0 = ri & (kBuf - 1);
        const int   i1 = (i0 + 1) & (kBuf - 1);
        return buf[i0] + (buf[i1] - buf[i0]) * (rp - (float)ri);
    }
    void Process(float &l, float &r, float amt)
    {
        const float rate = 0.20f - amt * 0.14f; // ~0.20 Hz .. ~0.06 Hz: slower the further it's turned up
        lfo_ += rate * (1.f / 44100.f);
        if(lfo_ >= 1.f)
            lfo_ -= 1.f;
        const float mod = 0.5f * (1.f - cosf(6.2831853f * lfo_)); // 0..1
        const float d   = 22.f + mod * amt * 310.f;               // ~0.5 ms .. ~7.5 ms at full depth
        const float fb  = amt * 0.45f;                            // light resonance, scales with the knob
        bufL[w_]        = l + fbL_ * fb;
        bufR[w_]        = r + fbR_ * fb;
        const float sl  = Read(bufL, (float)w_ - d);
        const float sr  = Read(bufR, (float)w_ - d);
        fbL_            = sl;
        fbR_            = sr;
        w_              = (w_ + 1) & (kBuf - 1);
        const float mix = amt * 0.5f; // up to 50% wet (classic flange)
        l               = l * (1.f - mix) + sl * mix;
        r               = r * (1.f - mix) + sr * mix;
    }
};

enum {
    P_TIME, P_FEEDBACK, P_DELAY, P_FBFILTER, P_FBRESO, P_SAT, P_SYNC,           // DELAY A (+ global SAT)
    P_SIZE, P_REVERB, P_DRIVE, P_WARBLE, P_DEGRADE, P_DUCK, P_MIX,              // SPACE / TAPE / OUT
    P_NOISE, P_NOISEFILT, P_NOISEMOD, P_CRACKLE, P_PHZRATE, P_PHZDEPTH,         // AIR / MOD
    P_DAMP, P_HIPASS,                                                          // SPACE (appended, restore by key)
    P_DRIFT,                                                                   // SPACE drift (Galactic vibrato)
    P_WIDTH,                                                                   // DELAY A width (ping-pong stereo + Haas)
    P_TIME2, P_FEEDBACK2, P_TONE2, P_DELAY2, P_SYNC2, P_WIDTH2,                 // DELAY B (own time/fb/tone/level/width)
    P_FLANGER,                                                                 // DELAY B flanger (one knob: depth + mix)
    P_PANIC,                                                                    // momentary kill: any tap flushes all tails
    P_COUNT
};
const char *const kKeys[P_COUNT] = {
    "time", "feedback", "delay", "fbfilter", "fbreso", "sat", "sync",
    "size", "reverb", "drive", "warble", "degrade", "duck", "mix",
    "noise", "noisefilt", "noisemod", "crackle", "phzrate", "phzdepth",
    "damp", "hipass", "drift", "width",
    "time2", "feedback2", "tone2", "delay2", "sync2", "width2", "flanger", "panic"};
// time is in ms (Free = that delay time; Sync = the knob picks the note division).
// feedback goes to 120 (100+ = self-oscillation); delay = echo send level; sat = tape saturation on repeats.
// noise = one filtered noise bed (noisefilt = dark..bright tone, noisemod = movement); crackle = vinyl pops;
// phz* = phaser rate/depth. damp = reverb damping (Galactic lowpass); hipass = HP on the reverb return.
// drift = reverb vibrato depth (Galactic's built-in chorus, exposed; default 40 = the old fixed lush gC=0.4).
// width/width2 = per-delay ping-pong stereo width (0 = mono .. 100 = full, + a small Haas offset for extra width).
// Delay B differs from A: tone2 = one bipolar LP/HP tilt (no resonance) instead of A's DJ filter + resonance.
// delay2 (B level) defaults 0 = off, so adding the second line doesn't change existing single-delay patches.
const float kDefaults[P_COUNT] = {1000.f, 45.f, 75.f, 0.f, 20.f, 0.f, 1.f, 55.f, 30.f, 20.f, 15.f, 0.f, 0.f, 50.f, 0.f, 50.f, 30.f, 0.f, 30.f, 0.f, 20.f, 0.f, 40.f, 100.f, 500.f, 35.f, 0.f, 0.f, 1.f, 100.f, 0.f, 0.f};
const float kMin[P_COUNT]      = {10.f, 0.f, 0.f, -100.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 10.f, 0.f, -100.f, 0.f, 0.f, 0.f, 0.f, 0.f};
const float kMax[P_COUNT]      = {2000.f, 120.f, 100.f, 100.f, 100.f, 100.f, 1.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 2000.f, 120.f, 100.f, 100.f, 1.f, 100.f, 100.f, 1.f};

struct DubStation
{
    airwindows::Galactic                        galactic;
    chompi::InterpolatedDelayLine               del, del2;
    chompi::InterpolatedDelayLine::AudioSample *del_mem, *del2_mem;
    DjFilter                                    fbfilter;
    chompi::Warble                              warble;
    Degrade                                     degrade;
    Phaser                                      phaser;
    Flanger                                     flangerB;
    DcBlock                                     dc_fb_l_, dc_fb_r_, dc_fb2_l_, dc_fb2_r_;

    float p[P_COUNT];
    float bpm;

    float sat_, sat_target_;
    float regen_, regen_target_;
    float regen2_, regen2_target_;              // Delay B feedback
    float width_a_, width_a_target_, width_b_, width_b_target_; // per-delay ping-pong width (0..1)
    float haasA_[kHaasMax]; int haasA_i_;                       // tiny inter-channel delay for extra width
    float haasB_[kHaasMax]; int haasB_i_;
    float mix_, mix_target_;
    float duck_amt_, duck_amt_target_;
    float dly_time_, dly_time_target_;
    float dly_time2_, dly_time2_target_;        // Delay B time (frames)
    float noise_amt_, noise_amt_target_;   // filtered noise bed level (squared)
    float crackle_amt_, crackle_amt_target_; // vinyl crackle level (0..1)
    float noise_a_, noise_ahp_, noise_gcomp_, noise_mod_; // noise LP coeff, HP coeff, level comp, movement depth
    float del_send_, del_send_target_;   // Delay A echo send level (DELAY knob)
    float del_send2_, del_send2_target_; // Delay B echo send level
    float rev_send_, rev_send_target_;   // parallel reverb send level (REVERB knob)
    float rev_hp_a_;                   // HP coeff on the reverb return (HIPASS knob)
    float rev_hp_l_, rev_hp_r_;        // HP filter state
    float phz_mix_, phz_mix_target_;   // phaser depth/mix
    float phz_rate_hz_;                // phaser LFO rate
    float flanger_, flanger_target_;   // Delay B flanger amount (one knob)

    float fbfilt_ctrl_, fbfilt_res_, res_comp_; // res_comp_ = A feedback-gain comp so high resonance can't run away
    float tone2_a_, tone2_mode_, tone2_l_, tone2_r_; // Delay B bipolar tone (one-pole LP/HP, no resonance)
    float dsat_; // delay/tape saturation on the repeats (global, on the summed tails)
    float deg_ratio_, deg_q_, deg_on_, deg_lp_;
    float gA_, gB_, gC_, gD_, gE_; // Galactic params

    float    duck_env_;
    int      flush_pending_, flush_cooldown_; // PANIC: deferred to the audio thread + debounced (see ApplyFx)
    float    fb_lp_l_, fb_lp_r_;   // one-pole LP in the feedback path (tape-style band-limit, tames aliasing)
    float    fb_lp2_l_, fb_lp2_r_; // Delay B feedback band-limit LP
    uint32_t rng_;
    float    noise_lp_l_, noise_lp_r_, noise_hp_l_, noise_hp_r_, crk_env_l_, crk_env_r_;
    float    noise_gust_;        // slow random-walk amplitude drift (keeps the noise bed moving)

    float frand()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (int32_t)rng_ * (1.0f / 2147483648.0f);
    }
    // one filtered noise voice: NOISEFILT sweeps BOTH a gentle one-pole LP (brightness) and a one-pole HP
    // that rises with the knob -> bright = a thin high-passed hiss, dark = fuller. gcomp keeps the level up.
    float NoiseSample(float &lp, float &hp)
    {
        const float w = frand();
        lp += noise_a_ * (w - lp);        // low-passed (brightness ceiling)
        hp += noise_ahp_ * (lp - hp);     // track the lows of that -> subtract = high-passed band
        return (lp - hp) * noise_gcomp_;
    }
    float CrackleSample(float &env, float density)
    {
        if(frand() > 1.f - density) // sparse random pop
            env = frand();
        const float out = env;
        env *= 0.5f;                // short decay -> a click a few samples long
        if(env > -1e-4f && env < 1e-4f)
            env = 0.f;
        return out;
    }

    // delay length in frames for a given delay time in ms (clamped to the buffer)
    float MsToFrames(float ms) const { return fclamp(ms * 0.001f * kSampleRate, kMinDelayTime, (float)kMaxDelayTime); }
    // Sync: map the knob's raw ms range evenly onto the 7 divisions (predictable, nudge-able);
    // the knob position itself stays the raw value, so it never snaps/springs back.
    int SyncDivIndexFor(float timeVal) const
    {
        const float n = (timeVal - 10.f) / 1990.f;
        int         d = (int)(n * 7.f);
        return d < 0 ? 0 : d > 6 ? 6 : d;
    }
    int SyncDivIndex() const { return SyncDivIndexFor(p[P_TIME]); }
    float DivMs(int d) const { return kDivBeats[d] * 60000.f / (bpm > 1.f ? bpm : 120.f); }

    void Apply()
    {
        if(p[P_SYNC] >= .5f)
            dly_time_target_ = MsToFrames(DivMs(SyncDivIndexFor(p[P_TIME]))); // TIME knob picks the division
        else
            dly_time_target_ = MsToFrames(p[P_TIME]);
        if(p[P_SYNC2] >= .5f)
            dly_time2_target_ = MsToFrames(DivMs(SyncDivIndexFor(p[P_TIME2])));
        else
            dly_time2_target_ = MsToFrames(p[P_TIME2]);

        regen_target_     = p[P_FEEDBACK] * .01f; // 0..1.2; >=1.0 self-oscillates (bounded by the loop tanh)
        regen2_target_    = p[P_FEEDBACK2] * .01f;
        width_a_target_   = p[P_WIDTH] * .01f;     // 0..1, 1 = full ping-pong, 0 = mono
        width_b_target_   = p[P_WIDTH2] * .01f;
        mix_target_       = p[P_MIX] * .01f;
        duck_amt_target_  = p[P_DUCK] * .01f;
        del_send_target_  = p[P_DELAY] * .01f;
        del_send2_target_ = p[P_DELAY2] * .01f;
        {
            const float n     = p[P_NOISE] * .01f;
            noise_amt_target_ = n * n; // squared: fine control at the quiet low end
            crackle_amt_target_ = p[P_CRACKLE] * .01f;
            // NOISEFILT: gentle one-pole LP (brightness) 40 Hz .. ~20 kHz, log sweep ...
            const float tone = p[P_NOISEFILT] * .01f;
            const float fc   = 40.f * powf(500.f, tone);
            noise_a_         = 1.f - expf(-6.2831853f * fc / kSampleRate);
            // ... plus a one-pole HP that climbs 50 Hz (dark, fuller) .. ~15 kHz (bright) so the top end is a
            // genuinely thin high-passed hiss (same shallow 6 dB/oct slope).
            const float fhp  = 50.f * powf(300.f, tone);
            noise_ahp_       = 1.f - expf(-6.2831853f * fhp / kSampleRate);
            noise_gcomp_     = sqrtf((2.f - noise_a_) / noise_a_); // LP level comp across the sweep
            noise_gcomp_    *= 1.f + 2.2f * tone;                  // make up the band-pass loss as the HP rises
            if(noise_gcomp_ > 10.f)
                noise_gcomp_ = 10.f;
            noise_mod_ = p[P_NOISEMOD] * .01f;
        }
        dsat_             = p[P_SAT] * .01f;

        phz_mix_target_ = p[P_PHZDEPTH] * .01f;
        {
            const float r = p[P_PHZRATE] * .01f;
            phz_rate_hz_  = 0.025f + r * r * 2.f; // 0.025 .. ~2 Hz: half speed, really slow at the bottom
        }
        flanger_target_ = p[P_FLANGER] * .01f; // Delay B flanger amount (one knob: depth + mix + feedback)

        const float drv = logf(1.7f * (p[P_DRIVE] * .01f) + 1.f);
        sat_target_      = drv * 13.f + 1.f;

        warble.SetFreq(p[P_WARBLE] * .01f);

        // DJ filter: knob full travel maps to ctrl [0.15, 0.85] (= old ±70) so the extremes stay audible
        // instead of fully closing the LP/HP; centre (0) is flat.
        fbfilt_ctrl_ = 0.5f + (p[P_FBFILTER] * .01f) * 0.35f;
        fbfilt_res_  = p[P_FBRESO] * .01f * 0.72f; // max resonance trimmed 10% (0.8->0.72) to ease aliasing
        // the resonant peak lives inside A's feedback loop, so high resonance lifts the loop gain and runs away.
        // Only the top half of the resonance knob gets compensated, so normal/low resonance leaves the feedback
        // (and the long 120% tails) fully intact; past ~50% it ramps in to stop the self-oscillation buildup.
        const float rx = fbfilt_res_ - 0.4f;
        res_comp_      = rx > 0.f ? 1.f / (1.f + rx * 1.0f) : 1.f; // 1.0 up to ~50% reso -> ~0.71 at full (gentle)
        // Delay B TONE: one bipolar one-pole. Centre = flat; left = low-pass down to 100 Hz,
        // right = high-pass up to 10 kHz (gentle 6 dB/oct, no resonance -> different character than A).
        {
            const float tv = p[P_TONE2] * .01f; // -1..1
            if(tv < -0.02f)
            {
                tone2_mode_    = -1.f;
                const float fc = 12000.f * powf(100.f / 12000.f, -tv); // 12 kHz .. 100 Hz
                tone2_a_       = 1.f - expf(-6.2831853f * fc / kSampleRate);
            }
            else if(tv > 0.02f)
            {
                tone2_mode_    = 1.f;
                const float fc = 20.f * powf(500.f, tv);               // 20 Hz .. 10 kHz
                tone2_a_       = 1.f - expf(-6.2831853f * fc / kSampleRate);
            }
            else
                tone2_mode_ = 0.f;
        }

        // Galactic runs 100% wet as a parallel SEND fed from the source (not the delay echoes); SIZE sets its
        // decay (A, inverted) and room size (D); REVERB is only the send level, mixed in after (no coupling).
        const float size01 = p[P_SIZE] * .01f;
        gA_ = 1.f - size01 * 0.9f;
        gB_ = 1.f - p[P_DAMP] * 0.009f;        // DAMP: 0 = open/bright (gB=1), 100 = dark/damped (gB~0.1)
        gC_ = p[P_DRIFT] * 0.01f;  // DRIFT: reverb vibrato depth (Galactic drift_ = C^3*0.001); 40 = old lush
        gD_ = 0.2f + size01 * 0.8f;
        gE_ = 1.f;
        rev_send_target_ = p[P_REVERB] * .05f; // up to 5.0: full REVERB reads loud (was .03/3.0, user wanted more)
        // HIPASS: one-pole HP on the reverb return, 20 Hz (off) .. ~1.5 kHz, log sweep
        const float fhp_rev = 20.f * powf(75.f, p[P_HIPASS] * .01f);
        rev_hp_a_ = 1.f - expf(-6.2831853f * fhp_rev / kSampleRate);

        const float dg = p[P_DEGRADE] * .01f;
        deg_on_          = dg > 0.0015f ? 1.f : 0.f;
        deg_ratio_       = 1.f - dg * 0.97f;               // sample & hold rate -> SR reduction, engages first
        const float bits = 16.f - 10.f * powf(dg, 1.5f);   // 16 -> 6 bits, comes in later than the SR drop
        deg_q_           = powf(2.f, bits) * 0.5f;
        // reconstruction LP tracks the decimation rate (cutoff ~ effective Nyquist of the held signal),
        // so the sample&hold images are filtered away -> vintage-sampler grit, not harsh aliasing
        deg_lp_          = 1.f - expf(-3.14159265f * deg_ratio_);
    }

    bool Init()
    {
        bpm = 120.f;
        del_mem = (chompi::InterpolatedDelayLine::AudioSample *)calloc(kMaxDelayTime, sizeof *del_mem);
        if(!del_mem)
            return false;
        del2_mem = (chompi::InterpolatedDelayLine::AudioSample *)calloc(kMaxDelayTime, sizeof *del2_mem);
        if(!del2_mem)
        {
            free(del_mem);
            return false;
        }

        galactic.Init();
        del.Init(del_mem, kMaxDelayTime);
        del2.Init(del2_mem, kMaxDelayTime);

        fbfilter.Init(kSampleRate);
        fbfilter.SetControl(.5f);
        fbfilter.SetRes(0.f);
        fbfilter.lp_ = fbfilter.lp_target_;
        fbfilter.hp_ = fbfilter.hp_target_;
        tone2_a_    = 0.f;
        tone2_mode_ = 0.f;
        tone2_l_ = tone2_r_ = 0.f;
        for(int i = 0; i < kHaasMax; i++)
            haasA_[i] = haasB_[i] = 0.f;
        haasA_i_ = haasB_i_ = 0;

        warble.Init(kSampleRate);
        degrade.Init();
        phaser.Init();
        flangerB.Init();
        dc_fb_l_.Init();
        dc_fb_r_.Init();
        dc_fb2_l_.Init();
        dc_fb2_r_.Init();
        duck_env_  = 0.f;
        flush_pending_ = 0;
        flush_cooldown_ = 0;
        fb_lp_l_ = fb_lp_r_ = 0.f;
        fb_lp2_l_ = fb_lp2_r_ = 0.f;
        rng_       = 2463534242u;
        noise_lp_l_ = noise_lp_r_ = noise_hp_l_ = noise_hp_r_ = crk_env_l_ = crk_env_r_ = 0.f;
        rev_hp_l_ = rev_hp_r_ = 0.f;
        noise_gust_ = 0.f;

        for(int i = 0; i < P_COUNT; i++)
            p[i] = kDefaults[i];
        Apply();
        sat_         = sat_target_;
        regen_       = regen_target_;
        regen2_      = regen2_target_;
        width_a_     = width_a_target_;
        width_b_     = width_b_target_;
        mix_         = mix_target_;
        duck_amt_    = duck_amt_target_;
        dly_time_    = dly_time_target_;
        dly_time2_   = dly_time2_target_;
        noise_amt_   = noise_amt_target_;
        crackle_amt_ = crackle_amt_target_;
        del_send_    = del_send_target_;
        del_send2_   = del_send2_target_;
        rev_send_    = rev_send_target_;
        phz_mix_     = phz_mix_target_;
        flanger_     = flanger_target_;
        return true;
    }

    // PANIC: silence every tail/feedback right now (delay buffers, reverb network, mod + filter states).
    // Params are left untouched; the next block re-applies them, so live input keeps flowing cleanly.
    void Flush()
    {
        if(del_mem)  memset(del_mem,  0, kMaxDelayTime * sizeof *del_mem);
        if(del2_mem) memset(del2_mem, 0, kMaxDelayTime * sizeof *del2_mem);
        galactic.Init();          // memsets its whole delay network, then restores defaults
        phaser.Init();
        flangerB.Init();
        dc_fb_l_.Init();  dc_fb_r_.Init();  dc_fb2_l_.Init();  dc_fb2_r_.Init();
        tone2_l_  = tone2_r_  = 0.f;
        for(int i = 0; i < kHaasMax; i++)
            haasA_[i] = haasB_[i] = 0.f;
        haasA_i_  = haasB_i_  = 0;
        duck_env_ = 0.f;
        fb_lp_l_  = fb_lp_r_  = 0.f;
        fb_lp2_l_ = fb_lp2_r_ = 0.f;
        noise_lp_l_ = noise_lp_r_ = noise_hp_l_ = noise_hp_r_ = crk_env_l_ = crk_env_r_ = 0.f;
        rev_hp_l_ = rev_hp_r_ = 0.f;
        noise_gust_ = 0.f;
    }

    void ApplyFx(float *outl, float *outr, size_t size)
    {
        // PANIC: run the requested flush here (audio thread, no race with set_param). The flush is a ~2.5 MB
        // memset (delay buffers + Galactic's reverb network) = one brief CPU spike, harmless on a real tap but
        // pathological if hammered, so it's debounced to at most once per 500 ms. One tap kills everything; a
        // second kill isn't needed sooner than that.
        if(flush_cooldown_ > 0)
            flush_cooldown_ -= (int)size;
        if(flush_pending_ && flush_cooldown_ <= 0)
        {
            Flush();
            flush_pending_  = 0;
            flush_cooldown_ = (int)(0.5f * kSampleRate);
        }

        float dryl[128], dryr[128], coll[128], colr[128];
        galactic.SetParams(gA_, gB_, gC_, gD_, gE_);
        fbfilter.SetControl(fbfilt_ctrl_);
        fbfilter.SetRes(fbfilt_res_);
        const float kFbLp = 0.66f; // one-pole LP ~7.5 kHz in the feedback path: band-limits the signal going
                                   // into the tanh so the resonant peak can't fold back as aliasing (darker, tape-like)

        for(size_t i = 0; i < size; i++)
        {
            fonepole(sat_, sat_target_, .001f);
            fonepole(regen_, regen_target_, .001f);
            fonepole(regen2_, regen2_target_, .001f);
            fonepole(width_a_, width_a_target_, .001f);
            fonepole(width_b_, width_b_target_, .001f);
            fonepole(dly_time_, dly_time_target_, .001f);
            fonepole(dly_time2_, dly_time2_target_, .001f);
            fonepole(noise_amt_, noise_amt_target_, .001f);
            fonepole(crackle_amt_, crackle_amt_target_, .001f);
            fonepole(del_send_, del_send_target_, .001f);
            fonepole(del_send2_, del_send2_target_, .001f);
            fonepole(phz_mix_, phz_mix_target_, .001f);
            fonepole(flanger_, flanger_target_, .001f);

            dryl[i] = outl[i];
            dryr[i] = outr[i];

            // tape colour (drive)
            float       wl = SoftClip(sat_ * outl[i]);
            float       wr = SoftClip(sat_ * outr[i]);
            const float g  = 1.f - SoftClip(.4f * (sat_ - 1.f)) * .7f;
            wl *= g;
            wr *= g;
            warble.Process(wl, wr, &wl, &wr);
            if(deg_on_ > .5f) // degrade is a tape colour on the source, like drive/warble (not just feedback)
                degrade.Process(deg_ratio_, deg_q_, deg_lp_, wl, wr);

            // the coloured source feeds the reverb send in parallel (independent of the delay)
            coll[i] = wl;
            colr[i] = wr;

            // the coloured source injected into both delay lines; each line ping-pongs via its own L/R cross-feedback
            const float srcL  = wl;
            const float srcR  = wr;
            // feedback ceiling: feedback >= 100% self-oscillates but sustains at a safe, constant level (no runaway).
            // Saturation is NOT in the feedback path (would compound tanh gain and run away) -- it's applied once
            // on the summed echo tails below.
            const float kCeil = 0.75f;

            // --- Delay A: tape delay, ping-pong, filtered + band-limited self-oscillating feedback ---
            del.SetDelay(dly_time_);
            const chompi::InterpolatedDelayLine::AudioSample rdA = del.Read();
            float flA = s162f(rdA.l);
            float frA = s162f(rdA.r);
            fbfilter.Process(flA, frA, &flA, &frA);
            flA = dc_fb_l_.Process(flA);
            frA = dc_fb_r_.Process(frA);
            fb_lp_l_ += kFbLp * (flA - fb_lp_l_); // tape-style band-limit: tames the highs the feedback keeps adding
            fb_lp_r_ += kFbLp * (frA - fb_lp_r_);
            flA = fb_lp_l_;
            frA = fb_lp_r_;
            {
                const float in_l = kCeil * tanhf((srcL + frA * regen_ * res_comp_) / kCeil); // cross L/R = ping-pong
                const float in_r = kCeil * tanhf((srcR + flA * regen_ * res_comp_) / kCeil);
                const chompi::InterpolatedDelayLine::AudioSample wsA
                    = {int16_t(f2s16(in_l)), int16_t(f2s16(in_r))};
                del.Write(wsA);
            }
            float elA = flA, erA = frA;
            WidthStereo(elA, erA, width_a_);
            erA = HaasTap(haasA_, haasA_i_, erA, (int)(width_a_ * 120.f)); // small R offset grows with width

            // --- Delay B: fully independent second line (own time/feedback/filter/pan) ---
            del2.SetDelay(dly_time2_);
            const chompi::InterpolatedDelayLine::AudioSample rdB = del2.Read();
            float flB = s162f(rdB.l);
            float frB = s162f(rdB.r);
            if(tone2_mode_ < -0.5f) // low-pass
            {
                tone2_l_ += tone2_a_ * (flB - tone2_l_); flB = tone2_l_;
                tone2_r_ += tone2_a_ * (frB - tone2_r_); frB = tone2_r_;
            }
            else if(tone2_mode_ > 0.5f) // high-pass
            {
                tone2_l_ += tone2_a_ * (flB - tone2_l_); flB -= tone2_l_;
                tone2_r_ += tone2_a_ * (frB - tone2_r_); frB -= tone2_r_;
            }
            flB = dc_fb2_l_.Process(flB);
            frB = dc_fb2_r_.Process(frB);
            fb_lp2_l_ += kFbLp * (flB - fb_lp2_l_);
            fb_lp2_r_ += kFbLp * (frB - fb_lp2_r_);
            flB = fb_lp2_l_;
            frB = fb_lp2_r_;
            {
                const float in_l = kCeil * tanhf((srcL + frB * regen2_) / kCeil);
                const float in_r = kCeil * tanhf((srcR + flB * regen2_) / kCeil);
                const chompi::InterpolatedDelayLine::AudioSample wsB
                    = {int16_t(f2s16(in_l)), int16_t(f2s16(in_r))};
                del2.Write(wsB);
            }
            float elB = flB, erB = frB;
            WidthStereo(elB, erB, width_b_);
            erB = HaasTap(haasB_, haasB_i_, erB, (int)(width_b_ * 120.f));
            if(flanger_ > 0.0005f) // flanger on Delay B's repeats only
                flangerB.Process(elB, erB, flanger_);

            // sum the two delays at their send levels
            float echoL = elA * del_send_ + elB * del_send2_;
            float echoR = erA * del_send_ + erB * del_send2_;
            if(dsat_ > 0.001f) // global tape saturation applied ONCE on the summed tails (not fed back -> no runaway)
            {
                const float drv = 1.f + dsat_ * 7.f;
                const float sg  = 1.f - dsat_ * 0.45f; // trim level so it doesn't get louder
                echoL = (echoL * (1.f - dsat_) + tanhf(echoL * drv) * dsat_) * sg;
                echoR = (echoR * (1.f - dsat_) + tanhf(echoR * drv) * dsat_) * sg;
            }
            if(phz_mix_ > 0.0005f) // phaser on the summed echoes only (classic phased delay; reverb stays clean)
                phaser.Process(echoL, echoR, phz_rate_hz_, phz_mix_);
            wl += echoL;
            wr += echoR;

            // ambient textures, independent of the reverb send; they stay audible even with no input,
            // so they form a bed that comes through at the MIX level.
            if(noise_amt_ > 0.0005f) // one filtered noise, always a little movement (more with NOISEMOD)
            {
                noise_gust_ += 0.0003f * (frand() - noise_gust_); // slow random-walk drift
                float gmod = 1.f + (0.15f + noise_mod_ * 1.3f) * noise_gust_ * 12.f;
                gmod       = gmod < 0.1f ? 0.1f : gmod > 2.f ? 2.f : gmod;
                const float lvl = noise_amt_ * 0.09f * gmod;
                wl += NoiseSample(noise_lp_l_, noise_hp_l_) * lvl;
                wr += NoiseSample(noise_lp_r_, noise_hp_r_) * lvl;
            }
            if(crackle_amt_ > 0.0005f)
            {
                const float dens = crackle_amt_ * 0.0012f; // sparser pops than before
                // L/R pops are independent (fully decorrelated = very wide); narrow the stereo image
                // via mid/side so a pop no longer lands hard on just one side.
                const float cl = CrackleSample(crk_env_l_, dens);
                const float cr = CrackleSample(crk_env_r_, dens);
                const float mid = 0.5f * (cl + cr);
                const float kWidth = 0.5f;                 // 0 = mono, 1 = full stereo
                wl += (mid + (cl - mid) * kWidth) * crackle_amt_ * 0.5f;
                wr += (mid + (cr - mid) * kWidth) * crackle_amt_ * 0.5f;
            }

            outl[i] = wl; // delay + textures wet (parallel reverb added below)
            outr[i] = wr;
        }

        // parallel reverb: 100% wet reverb of the coloured source only (not the delay, not the noise)
        galactic.Process(coll, colr, coll, colr, (int)size);

        for(size_t i = 0; i < size; i++)
        {
            fonepole(duck_amt_, duck_amt_target_, .001f);
            fonepole(mix_, mix_target_, .001f);
            fonepole(rev_send_, rev_send_target_, .0005f);

            // HIPASS on the reverb return: subtract the tracked lows -> thins the tail's low rumble
            rev_hp_l_ += rev_hp_a_ * (coll[i] - rev_hp_l_);
            rev_hp_r_ += rev_hp_a_ * (colr[i] - rev_hp_r_);
            const float revl = coll[i] - rev_hp_l_;
            const float revr = colr[i] - rev_hp_r_;

            const float wetl = outl[i] + revl * rev_send_;
            const float wetr = outr[i] + revr * rev_send_;

            const float lvl   = (fabsf(dryl[i]) + fabsf(dryr[i])) * .5f;
            const float coeff = lvl > duck_env_ ? 0.3f : 0.0015f;
            duck_env_ += coeff * (lvl - duck_env_);
            float dg = 1.f - duck_amt_ * (duck_env_ * 4.f > 1.f ? 1.f : duck_env_ * 4.f);
            if(dg < 0.f)
                dg = 0.f;

            outl[i] = (1.f - mix_) * dryl[i] + mix_ * (wetl * dg);
            outr[i] = (1.f - mix_) * dryr[i] + mix_ * (wetr * dg);
        }
    }
};

inline int16_t ToInt16(float f)
{
    if(!(f == f))
        return 0;
    f *= 32768.f;
    return f >= 32767.f ? 32767 : f <= -32768.f ? -32768 : (int16_t)lrintf(f);
}

int KeyIndex(const char *key)
{
    for(int i = 0; i < P_COUNT; i++)
        if(!strcmp(key, kKeys[i]))
            return i;
    return -1;
}

void *dc_create(const char *)
{
    void *mem = calloc(1, sizeof(DubStation));
    if(!mem)
        return NULL;
    DubStation *t = new(mem) DubStation;
    if(!t->Init())
    {
        free(mem);
        return NULL;
    }
    return t;
}

void dc_destroy(void *inst)
{
    DubStation *t = (DubStation *)inst;
    if(!t)
        return;
    free(t->del_mem);
    free(t->del2_mem);
    free(t);
}

void dc_midi(void *, const uint8_t *, int) {}

void SetValue(DubStation *t, int i, float v)
{
    if(!(v == v))
        return;
    t->p[i] = v < kMin[i] ? kMin[i] : v > kMax[i] ? kMax[i] : v;
}

void RestoreState(DubStation *t, const char *s)
{
    while(*s)
    {
        while(*s == ' ')
            s++;
        const char *eq = strchr(s, '=');
        if(!eq)
            break;
        for(int i = 0; i < P_COUNT; i++)
            if(strlen(kKeys[i]) == (size_t)(eq - s) && !strncmp(s, kKeys[i], eq - s))
                SetValue(t, i, (float)atof(eq + 1));
        s = eq + 1;
        while(*s && *s != ' ')
            s++;
    }
}

void dc_set_param(void *inst, const char *key, const char *val)
{
    DubStation *t = (DubStation *)inst;
    if(!val)
        return;
    if(!strcmp(key, "state"))
    {
        RestoreState(t, val);
        t->Apply();
        return;
    }
    if(!strcmp(key, "lfo_bpm"))
    {
        const float b = (float)atof(val);
        if(b > 1.f)
            t->bpm = b;
        t->Apply();
        return;
    }
    const int i = KeyIndex(key);
    if(i < 0)
        return;
    SetValue(t, i, (float)atof(val));
    if(i == P_PANIC)   // any tap of the Panic toggle requests a flush; the audio thread does it (debounced)
        t->flush_pending_ = 1;
    t->Apply();
}

int dc_get_param(void *inst, const char *key, char *buf, int buf_len)
{
    DubStation *t = (DubStation *)inst;
    if(!strcmp(key, "state"))
    {
        int len = 0;
        for(int k = 0; k < P_COUNT && len < buf_len; k++)
            len += snprintf(buf + len, buf_len - len, "%s%s=%g", k ? " " : "", kKeys[k], t->p[k]);
        return len < buf_len ? len : 0;
    }
    const int i = KeyIndex(key);
    if(i < 0)
        return 0;
    if(i == P_SYNC || i == P_SYNC2)
        return snprintf(buf, buf_len, "%d", t->p[i] >= .5f ? 1 : 0);
    if(i == P_TIME || i == P_TIME2)
    {
        // "<rawValue>\x1f<text>": the wrapper takes the leading raw ms for the knob position (so the
        // knob never springs back), and shows only <text>. In Sync that text is the chosen division.
        const int syncIdx = (i == P_TIME) ? P_SYNC : P_SYNC2;
        if(t->p[syncIdx] >= .5f)
        {
            const int d = t->SyncDivIndexFor(t->p[i]);
            return snprintf(buf, buf_len, "%d\x1f%s (%d ms)", (int)(t->p[i] + 0.5f), kDivLabels[d],
                            (int)(t->DivMs(d) + 0.5f));
        }
        return snprintf(buf, buf_len, "%d ms", (int)(t->p[i] + 0.5f));
    }
    if(i == P_FBFILTER)
    {
        // Delay A DJ filter, shown like B's tone: "<raw>\x1f<text>" keeps the knob position, text shows the
        // LP/HP corner. Mirror DJFilter::SetControl so the readout matches what the filter actually does;
        // corner Hz from the one-pole coeff g via fc = -(fs/2pi)*ln(1-g).
        const int   raw  = (int)lroundf(t->p[P_FBFILTER]);
        const float ctrl = 0.5f + (t->p[P_FBFILTER] * .01f) * 0.35f; // [0.15, 0.85]
        float glp = .01f + ctrl * 2.f;  if(glp > .90f) glp = .90f; if(glp < 0.f) glp = 0.f; glp = glp * glp * glp;
        float ghp = ctrl * 1.9f - 1.f;  if(ghp > 1.f)  ghp = 1.f;  if(ghp < 0.f) ghp = 0.f; ghp = ghp * ghp * ghp;
        if(ghp > 1e-4f)
        {
            const float fc = -(kSampleRate / 6.2831853f) * logf(1.f - ghp);
            if(fc >= 1000.f)
                return snprintf(buf, buf_len, "%d\x1fHP %.1f kHz", raw, (double)(fc / 1000.f));
            return snprintf(buf, buf_len, "%d\x1fHP %d Hz", raw, (int)(fc + 0.5f));
        }
        if(glp < .728f) // .729 = wide open; below that the LP is actually closing
        {
            const int fc = (int)(-(kSampleRate / 6.2831853f) * logf(1.f - glp) + 0.5f);
            return snprintf(buf, buf_len, "%d\x1fLP %d Hz", raw, fc);
        }
        return snprintf(buf, buf_len, "%d\x1f" "Flat", raw);
    }
    if(i == P_TONE2)
    {
        // "<raw>\x1f<text>": raw value keeps the knob position; text shows the LP/HP cutoff (or Flat).
        const float tv  = t->p[P_TONE2] * .01f;
        const int   raw = (int)lroundf(t->p[P_TONE2]);
        if(tv < -0.02f)
        {
            const int fc = (int)(12000.f * powf(100.f / 12000.f, -tv) + 0.5f);
            return snprintf(buf, buf_len, "%d\x1fLP %d Hz", raw, fc);
        }
        if(tv > 0.02f)
        {
            const float fc = 20.f * powf(500.f, tv);
            if(fc >= 1000.f)
                return snprintf(buf, buf_len, "%d\x1fHP %.1f kHz", raw, (double)(fc / 1000.f));
            return snprintf(buf, buf_len, "%d\x1fHP %d Hz", raw, (int)(fc + 0.5f));
        }
        return snprintf(buf, buf_len, "%d\x1f" "Flat", raw);
    }
    return snprintf(buf, buf_len, "%g", t->p[i]);
}

void dc_render(void *, int16_t *out_lr, int frames)
{
    memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
}

void dc_process(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames)
{
    DubStation *t = (DubStation *)inst;
    float       l[128], r[128];
    while(frames > 0)
    {
        const int n = frames > 128 ? 128 : frames;
        for(int i = 0; i < n; i++)
        {
            l[i] = in_lr[2 * i] * (1.f / 32768.f);
            r[i] = in_lr[2 * i + 1] * (1.f / 32768.f);
        }
        t->ApplyFx(l, r, n);
        for(int i = 0; i < n; i++)
        {
            out_lr[2 * i]     = ToInt16(l[i]);
            out_lr[2 * i + 1] = ToInt16(r[i]);
        }
        in_lr += 2 * n;
        out_lr += 2 * n;
        frames -= n;
    }
}

const mpc_engine_t kEngine = {dc_create, dc_destroy, dc_midi, dc_set_param, dc_get_param, dc_render, dc_process};
} // namespace

extern "C" const mpc_engine_t *mpc_engine(void)
{
    return &kEngine;
}
