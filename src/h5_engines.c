/*
 * h5_engines.c - per-voice DSP engines.
 *
 * Contains the Kick, Tom and Snare engines. Engine-only controls that are not
 * part of the public surface are fixed at their documented defaults:
 *
 *   Drive          0.15
 *   Filter Bend    0.50
 *   Resonance      1.00
 *   Volume         0.50
 *   Gain           1.00
 *
 * The noise generator runs freely across triggers; each trigger restarts the
 * oscillator phase, the envelopes and the filters. Tom's and Snare's
 * NoiseTone and Click are power-law controls (x^0.7), and each has a separate
 * transient envelope over filtered noise.
 *
 * Additional fixed 50 Hz / 20 kHz one-pole sections and a 338.6 Hz section
 * before the Kick/Tom swept resonant low-pass are not implemented yet; both
 * engines use the swept low-pass plus a fixed second-order output high-pass
 * (15 Hz for Kick, 25 Hz for Tom).
 */

#include "h5_internal.h"

#define H5_TOM_DRIVE_Q16       (15 * H5_Q16_ONE / 100)
#define H5_TOM_FILTER_BEND_Q16 (H5_Q16_ONE / 2)
#define H5_TOM_RESONANCE_Q16   H5_Q16_ONE
#define H5_TOM_OUT_HP_HZ       25
#define H5_TOM_OUT_Q16         (46341) /* 1/sqrt(2) */
#define H5_TOM_VOLUME_Q16      (H5_Q16_ONE / 2)

/* Cutoff sweep contribution at env = 1, Hz in Q16. It is folded with
 * gain^4; the library renders full-velocity hits, so gain = 1. */
#define H5_TOM_NOISE_SWEEP_Q16 (2000 * H5_Q16_ONE)

/* Rattle/noise weight: 0.3 * (1 - NoiseTone)^0.7. */
#define H5_TOM_NOISE_WEIGHT_Q16 (3 * H5_Q16_ONE / 10)

/* 5 ms body attack and 1 ms transient attack. */
#define H5_MS_Q16(ms) (((ms) * H5_Q16_ONE + 500) / 1000)

/*
 * Kick engine.
 *
 * Signal flow:
 *   env_n  = AR(0.2*NoiseDecay + 0.1, NoiseDecay)
 *   fc_n   = (NoisePitch + 2000*gain^4 * env_n^3)
 *            * (1 - FilterBend * (1 - env_n^3))
 *   nf     = SVF(u_hold; fc_n, Q)
 *   noise  = nf * env_n * Click^0.7 * (1-NoiseTone)^0.7
 *
 *   f      = TonePitch * 1.5^(Bend * env_t / 12)
 *   s      = sin(2*pi*phi), s2 = sin((pi/2)*s)
 *   env_t  = AR(0, Decay)
 *   tone   = env_t * (1-Click)^0.7 * 0.9 * NoiseTone^0.7
 *            * ((1-Drive)*s + Drive*0.5*(s2 + s))
 *
 *   y      = tanh(1.5 * (tone + noise))
 *   out    = HP15_SVF(y * 1.10479; Q = 1/sqrt(2)) * Volume * Gain
 *
 * The single Decay knob drives both engine decays: the wrapper writes the same
 * value to ToneDecay and NoiseDecay. The noise attack is 0.2*NoiseDecay + 0.1.
 *
 * Engine-only controls fixed at their documented defaults:
 *   Drive          0.15
 *   Filter Bend    1.00
 *   Resonance      1.00
 *   Volume         0.50
 *   Gain           1.00
 *
 * The Kick pitch envelope uses base 1.5, not the usual 2.0 semitone ratio.
 */

#define H5_KICK_DRIVE_Q16       (15 * H5_Q16_ONE / 100)
#define H5_KICK_RESONANCE_Q16   H5_Q16_ONE
#define H5_KICK_VOLUME_Q16      (H5_Q16_ONE / 2)
#define H5_KICK_NOISE_SWEEP_Q16 (2000 * H5_Q16_ONE)

/* log2(1.5) in Q16.16. Scaling the bend depth by this converts the engine's
 * 1.5^(n/12) curve into 2^(n*log2(1.5)/12), which the semitone table covers. */
#define H5_BEND_LOG2_3_2_Q16    (38336)

/* The tone path carries a fixed 0.9 level factor. */
#define H5_KICK_TONE_LEVEL_Q16  (58982)

/* Fixed output stage: second-order high-pass at 15 Hz, Butterworth Q, with a
 * 1.10479 input gain. */
#define H5_KICK_OUT_HP_HZ       15
#define H5_KICK_OUT_Q16         (46341) /* 1/sqrt(2) */
#define H5_KICK_OUT_GAIN_Q16    (72404) /* 1.10479 */

void h5_kick_init(struct h5_kick_s *k, uint32_t sample_rate)
{
    h5_env_init(&k->env_noise);
    h5_env_init(&k->env_tone);
    h5_noise_init_voice(&k->noise, H5_VOICE_KICK, sample_rate);
    h5_svf_reset(&k->noise_filter);
    h5_svf_reset(&k->out_filter);
    k->phase = 0u;
    k->base_inc = 0;
    k->bend_q16 = 0;
    k->noise_pitch_q16 = 0;
    k->noise_tone_q16 = 0;
    k->tone_gain_q16 = 0;
    k->click_gain_q16 = 0;
    k->active = 0u;
}

void h5_kick_configure(struct h5_kick_s *k,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    int64_t inc;
    int32_t click, decay, noise_decay, noise_attack;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    k->bend_q16 = params[H5_PARAM_BEND];
    k->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];
    /* NoiseTone is the tone amount: 1 selects the tone, 0 selects the noise.
     * The tone path carries the fixed 0.9 level factor and the (1-Click)^0.7
     * body law; the noise path carries (1-NoiseTone)^0.7. */
    k->noise_tone_q16 = h5_pow07_q16(
        H5_Q16_ONE - params[H5_PARAM_NOISE_TONE]);
    k->click_gain_q16 = h5_pow07_q16(click);
    k->tone_gain_q16 = h5_mul_q16(
        h5_pow07_q16(params[H5_PARAM_NOISE_TONE]), H5_KICK_TONE_LEVEL_Q16);
    k->tone_gain_q16 = h5_mul_q16(k->tone_gain_q16,
        h5_pow07_q16(H5_Q16_ONE - click));

    /* cycles/sample = pitch/rate; the 28-bit phase unit adds 2^28. pitch is
     * Q16 Hz, so the numerator is pitch << 12. */
    inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;
    if (inc < 1) {
        inc = 1;
    }
    k->base_inc = (int32_t)inc;

    /* The wrapper writes the single Decay value to both engine decays, so
     * NoiseDecay = ToneDecay = Decay. The noise attack is
     * 0.2*NoiseDecay + 0.1 seconds. */
    noise_decay = decay;
    noise_attack = (2 * decay) / 10 + H5_Q16_ONE / 10;
    h5_env_set_times(&k->env_noise, noise_attack, noise_decay, sample_rate);
    h5_env_set_times(&k->env_tone, 0, decay, sample_rate);

    h5_svf_tune(&k->noise_filter, H5_SVF_LP, params[H5_PARAM_NOISE_PITCH],
        H5_KICK_RESONANCE_Q16, sample_rate);
    h5_svf_tune(&k->out_filter, H5_SVF_HP,
        H5_KICK_OUT_HP_HZ * H5_Q16_ONE, H5_KICK_OUT_Q16, sample_rate);
}

void h5_kick_trigger(struct h5_kick_s *k)
{
    k->phase = 0u;
    h5_env_trigger(&k->env_noise);
    h5_env_trigger(&k->env_tone);
    h5_svf_reset(&k->noise_filter);
    h5_svf_reset(&k->out_filter);
    k->active = 1u;
}

int32_t h5_kick_process(struct h5_kick_s *k, uint32_t sample_rate)
{
    int32_t env_n, env_t, env3, env3_q16, semitones_q16, ratio;
    int32_t inc, s, s2, half, base, cutoff, u, nf, tone, noise, sum;
    int32_t blend, y, out;

    if (!k->active) {
        return 0;
    }

    env_n = h5_env_next(&k->env_noise);
    env_t = h5_env_next(&k->env_tone);
    if (env_n <= 0 && env_t <= 0
        && k->env_noise.state == H5_ENV_DONE
        && k->env_tone.state == H5_ENV_DONE) {
        k->active = 0u;
        return 0;
    }

    env3 = h5_cube_q30(env_n);
    env3_q16 = env3 >> 14;

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env_t / 12). Convert the
     * exponent to table semitones: n * log2(1.5), then 2^(n/12). */
    semitones_q16 = (int32_t)h5_shr_round((int64_t)k->bend_q16 * env_t, 30u);
    semitones_q16 = (int32_t)h5_shr_round(
        (int64_t)semitones_q16 * H5_BEND_LOG2_3_2_Q16, 16u);
    ratio = h5_semitone_ratio_interp(semitones_q16);
    inc = (int32_t)h5_shr_round(h5_mulwide(k->base_inc, ratio), 16u);
    if (inc < 1) {
        inc = 1;
    }
    k->phase = (k->phase + (uint32_t)inc) & 0x0FFFFFFFu;

    s = h5_sine_interp(k->phase);
    /* s2 = sin((pi/2) * s); a quarter of s's normalized value is 2^26 of a
     * 28-bit phase, i.e. s >> 5 for a Q31 sample. */
    s2 = h5_sine_interp((uint32_t)(s >> 5));

    /* fc_n = (NoisePitch + 2000*gain^4*env^3)
     *        * (1 - FilterBend*(1 - env^3))
     * Gain is fixed at 1.0. FilterBend is fixed at its Kick default 1.0, so
     * the bend factor (1 - FilterBend*(1 - env^3)) collapses to env^3 and
     * fc_n = (NoisePitch + 2000*gain^4*env^3) * env^3. */
    base = h5_sat_i32((int64_t)k->noise_pitch_q16
        + h5_mul_q16(H5_KICK_NOISE_SWEEP_Q16, env3_q16));
    cutoff = h5_mul_q16(base, env3_q16);

    h5_svf_tune(&k->noise_filter, H5_SVF_LP, cutoff, H5_KICK_RESONANCE_Q16,
        sample_rate);
    u = h5_noise_next(&k->noise);
    nf = h5_svf_run(&k->noise_filter, u);

    /* Noise/click: nf * env_n * Click^0.7 * (1-NoiseTone)^0.7. */
    noise = h5_mul_q30(nf, env_n);
    noise = h5_mul_q16(noise, k->click_gain_q16);
    noise = h5_mul_q16(noise, k->noise_tone_q16);

    /* Tone: env_t * (1-Click)^0.7 * 0.9*NoiseTone^0.7
     *       * ((1-Drive)*s + Drive*0.5*(s2 + s)). */
    half = (int32_t)(((int64_t)s2 + (int64_t)s) / 2);
    blend = h5_sat_add_q31(
        h5_mul_q16(s, H5_Q16_ONE - H5_KICK_DRIVE_Q16),
        h5_mul_q16(half, H5_KICK_DRIVE_Q16));
    tone = h5_mul_q30(blend, env_t);
    tone = h5_mul_q16(tone, k->tone_gain_q16);

    sum = h5_sat_add_q31(tone, noise);
    y = h5_softclip_q31(sum);

    /* Fixed output stage: 1.10479 input gain into the 15 Hz high-pass. */
    y = h5_mul_q16(y, H5_KICK_OUT_GAIN_Q16);
    out = h5_svf_run(&k->out_filter, y);
    out = h5_mul_q16(out, H5_KICK_VOLUME_Q16);
    /* Previous operation: out = h5_mul_q16(out, H5_KICK_GAIN_Q16);
     * The actual formula is out = volume * gain. Gain is fixed at 1.0, so
     * this multiply is an identity and is elided. Restore it if the gain
     * value ever changes. */

    return out;
}

static int32_t h5_pow10_q30(int32_t v)
{
    int64_t sq = h5_shr_round((int64_t)v * v, 30u);
    int32_t v2 = (int32_t)sq;
    int32_t v4 = (int32_t)h5_shr_round((int64_t)v2 * v2, 30u);
    int32_t v8 = (int32_t)h5_shr_round((int64_t)v4 * v4, 30u);

    return (int32_t)h5_shr_round((int64_t)v8 * v2, 30u);
}

void h5_tom_init(struct h5_tom_s *t, h5_voice voice, uint32_t sample_rate)
{
    h5_env_init(&t->env);
    h5_env_init(&t->env_click);
    h5_noise_init_voice(&t->noise, voice, sample_rate);
    h5_svf_reset(&t->noise_filter);
    h5_svf_reset(&t->out_filter);
    t->phase = 0u;
    t->base_inc = 0;
    t->bend_q16 = 0;
    t->noise_pitch_q16 = 0;
    t->tone_gain_q16 = 0;
    t->noise_gain_q16 = 0;
    t->body_gain_q16 = 0;
    t->click_gain_q16 = 0;
    t->active = 0u;
}

void h5_tom_configure(struct h5_tom_s *t,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    int64_t inc;
    int32_t noise_tone, click, decay;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    noise_tone = params[H5_PARAM_NOISE_TONE];
    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    t->bend_q16 = params[H5_PARAM_BEND];
    t->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];

    /* NoiseTone is the tone amount: 1 selects the tone, 0 selects the noise.
     * The noise side carries the 0.3 headroom factor. */
    t->tone_gain_q16 = h5_pow07_q16(noise_tone);
    t->noise_gain_q16 = h5_mul_q16(
        h5_pow07_q16(H5_Q16_ONE - noise_tone), H5_TOM_NOISE_WEIGHT_Q16);
    t->body_gain_q16 = h5_pow07_q16(H5_Q16_ONE - click);
    t->click_gain_q16 = h5_pow07_q16(click);

    /* cycles/sample = pitch/rate; the 28-bit phase unit adds 2^28. pitch is
     * Q16 Hz, so the numerator is pitch << 12. */
    inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;
    if (inc < 1) {
        inc = 1;
    }
    t->base_inc = (int32_t)inc;

    /* Body envelope: AR^3, 5 ms attack and Decay release. Transient
     * envelope: AR^10, 1 ms attack and 0.1*(Decay+1) release. */
    h5_env_set_times(&t->env, H5_MS_Q16(5), decay, sample_rate);
    h5_env_set_times(&t->env_click, H5_MS_Q16(1),
        (decay + H5_Q16_ONE + 5) / 10, sample_rate);

    h5_svf_tune(&t->noise_filter, H5_SVF_LP, params[H5_PARAM_NOISE_PITCH],
        H5_TOM_RESONANCE_Q16, sample_rate);
    h5_svf_tune(&t->out_filter, H5_SVF_HP,
        H5_TOM_OUT_HP_HZ * H5_Q16_ONE, H5_TOM_OUT_Q16, sample_rate);
}

void h5_tom_trigger(struct h5_tom_s *t)
{
    t->phase = 0u;
    h5_env_trigger(&t->env);
    h5_env_trigger(&t->env_click);
    h5_svf_reset(&t->noise_filter);
    h5_svf_reset(&t->out_filter);
    t->active = 1u;
}

int32_t h5_tom_process(struct h5_tom_s *t, uint32_t sample_rate)
{
    int32_t env, env_click, env3, env3_q16, one_minus, semitones_q16, ratio;
    int32_t inc, s, s2, sweep, base, cutoff, u, nf, tone, noise, sum;
    int32_t body, click, mix, half, blended, y, out;

    if (!t->active) {
        return 0;
    }

    env = h5_env_next(&t->env);
    env_click = h5_env_next(&t->env_click);
    if (env <= 0 && env_click <= 0
        && t->env.state == H5_ENV_DONE
        && t->env_click.state == H5_ENV_DONE) {
        t->active = 0u;
        return 0;
    }

    env3 = h5_cube_q30(env);
    env3_q16 = env3 >> 14;

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env / 12). */
    semitones_q16 = (int32_t)h5_shr_round((int64_t)t->bend_q16 * env, 30u);
    semitones_q16 = (int32_t)h5_shr_round(
        (int64_t)semitones_q16 * H5_BEND_LOG2_3_2_Q16, 16u);
    ratio = h5_semitone_ratio_interp(semitones_q16);
    inc = (int32_t)h5_shr_round(h5_mulwide(t->base_inc, ratio), 16u);
    if (inc < 1) {
        inc = 1;
    }
    t->phase = (t->phase + (uint32_t)inc) & 0x0FFFFFFFu;

    s = h5_sine_interp(t->phase);
    /* s2 = sin((pi/2) * s); a quarter of s's normalized value is 2^26 of a
     * 28-bit phase, i.e. s >> 5 for a Q31 sample. */
    s2 = h5_sine_interp((uint32_t)(s >> 5));

    /* fc_n = (NoisePitch + 2000*gain^4*env^3)
     *        * (1 - FilterBend*(1 - env^3))
     * Gain is fixed at 1.0 and FilterBend at its Tom default 0.5. */
    one_minus = H5_Q16_ONE - env3_q16;
    sweep = h5_mul_q16(H5_TOM_FILTER_BEND_Q16, one_minus);
    base = h5_sat_i32((int64_t)t->noise_pitch_q16
        + h5_mul_q16(H5_TOM_NOISE_SWEEP_Q16, env3_q16));
    cutoff = h5_mul_q16(base, H5_Q16_ONE - sweep);

    h5_svf_tune(&t->noise_filter, H5_SVF_LP, cutoff, H5_TOM_RESONANCE_Q16,
        sample_rate);
    u = h5_noise_next(&t->noise);
    nf = h5_svf_run(&t->noise_filter, u);

    /* Body: env^3 * (1-Click)^0.7 * (NoiseTone^0.7 * s
     *                               + 0.3*(1-NoiseTone)^0.7 * noise)
     * Transient: env_click^10 * Click^0.7 * noise. The transient is also
     * scaled by 0.4 + 0.6*gain^4, which is 1.0 at the full-velocity gain
     * this library renders. */
    tone = h5_mul_q16(s, t->tone_gain_q16);
    noise = h5_mul_q16(nf, t->noise_gain_q16);
    sum = h5_sat_add_q31(tone, noise);
    body = h5_mul_q30(sum, env3);
    body = h5_mul_q16(body, t->body_gain_q16);

    click = h5_mul_q16(nf, t->click_gain_q16);
    click = h5_mul_q30(click, h5_pow10_q30(env_click));
    mix = h5_sat_add_q31(body, click);

    /* The sine-shaper branch rides the body envelope and Click scaling so
     * the whole hit decays rather than cutting off at the release end. */
    half = h5_mul_q30((int32_t)(((int64_t)s2 + (int64_t)s) / 2), env3);
    half = h5_mul_q16(half, t->body_gain_q16);
    blended = h5_sat_add_q31(
        h5_mul_q16(mix, H5_Q16_ONE - H5_TOM_DRIVE_Q16),
        h5_mul_q16(half, H5_TOM_DRIVE_Q16));
    y = h5_softclip_q31(blended);

    out = h5_svf_run(&t->out_filter, y);
    out = h5_mul_q16(out, H5_TOM_VOLUME_Q16);
    /* Previous operation: out = h5_mul_q16(out, H5_TOM_GAIN_Q16);
     * The actual formula is out = volume * gain. Gain is fixed at 1.0, so
     * this multiply is an identity and is elided. Restore it if the gain
     * value ever changes. */

    return out;
}

/*
 * Snare engine.
 *
 * Signal flow:
 *   f       = TonePitch * 1.5^(Bend * env / 12)
 *   s       = sin(2*pi*phi), s2 = sin((pi/2)*s)
 *   rattle  = sum of 4 BP_SVF(u_hold; f_k, Q_k)
 *   body    = env^3 * (1-Click)^0.7
 *             * (NoiseTone^0.7 * s + 0.3*(1-NoiseTone)^0.7 * rattle)
 *   click   = env_click^10 * Click^0.7 * rattle
 *   mix     = body + click
 *   half    = env^3 * (1-Click)^0.7 * 0.5*(s2 + s)
 *   y       = tanh(1.5 * ((1-Drive)*mix + Drive*half))
 *   out     = LP_SVF(y; NoisePitch * sweep contour, Q) * Volume * Gain
 *
 * Sweep lowers the output cutoff as the body decays; at 0 the cutoff is
 * fixed, at 1 it falls by half the base cutoff over the release.
 *
 * Engine-only controls fixed at their documented defaults:
 *   Drive          0.15
 *   Volume         0.50
 *   Gain           1.00
 *   Output Q       1.00
 *   Body attack    1 ms
 *   Click attack   1 ms
 */

#define H5_SNARE_VOLUME_Q16      (H5_Q16_ONE / 2)
#define H5_SNARE_OUT_Q16         H5_Q16_ONE
#define H5_SNARE_ATTACK_Q16      H5_MS_Q16(1)
#define H5_SNARE_CLICK_ATTACK_Q16 H5_MS_Q16(1)
#define H5_SNARE_NOISE_WEIGHT_Q16 (3 * H5_Q16_ONE / 10)
#define H5_SNARE_SWEEP_DEPTH_Q16 (H5_Q16_ONE / 2)
#define H5_SNARE_DRIVE_Q16       (15 * H5_Q16_ONE / 100)

/* Four fixed band-pass resonators that shape the noise into a snare-wire
 * rattle. The engine's exact constants are not recovered yet; this band gives
 * the measured rattle its mid/high character without over-brightening the
 * default hit. */
#define H5_SNARE_RATTLE_FREQ_1 (800 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_2 (1200 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_3 (2000 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_4 (6000 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_Q16    (3 * H5_Q16_ONE / 2)

void h5_snare_init(struct h5_snare_s *sn, uint32_t sample_rate)
{
    size_t i;

    h5_env_init(&sn->env);
    h5_env_init(&sn->env_click);
    h5_noise_init_voice(&sn->noise, H5_VOICE_SNARE, sample_rate);
    for (i = 0u; i < 4u; i++) {
        h5_svf_reset(&sn->rattle[i]);
    }
    h5_svf_reset(&sn->out_filter);
    sn->phase = 0u;
    sn->base_inc = 0;
    sn->bend_q16 = 0;
    sn->noise_pitch_q16 = 0;
    sn->tone_gain_q16 = 0;
    sn->noise_gain_q16 = 0;
    sn->body_gain_q16 = 0;
    sn->click_gain_q16 = 0;
    sn->sweep_q16 = 0;
    sn->active = 0u;
}

void h5_snare_configure(struct h5_snare_s *sn,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    int64_t inc;
    int32_t noise_tone, click, decay;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    noise_tone = params[H5_PARAM_NOISE_TONE];
    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    sn->bend_q16 = params[H5_PARAM_BEND];
    sn->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];
    sn->sweep_q16 = params[H5_PARAM_SWEEP];

    /* NoiseTone is the tone amount: 1 selects the tone, 0 selects the rattle.
     * The rattle side carries the 0.3 headroom factor. */
    sn->tone_gain_q16 = h5_pow07_q16(noise_tone);
    sn->noise_gain_q16 = h5_mul_q16(
        h5_pow07_q16(H5_Q16_ONE - noise_tone), H5_SNARE_NOISE_WEIGHT_Q16);
    sn->body_gain_q16 = h5_pow07_q16(H5_Q16_ONE - click);
    sn->click_gain_q16 = h5_pow07_q16(click);

    /* cycles/sample = pitch/rate; the 28-bit phase unit adds 2^28. pitch is
     * Q16 Hz, so the numerator is pitch << 12. */
    inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;
    if (inc < 1) {
        inc = 1;
    }
    sn->base_inc = (int32_t)inc;

    /* Body envelope: AR^3 with a 1 ms attack and Decay release. Transient
     * envelope: AR^10 with a 1 ms attack and 0.2*Decay + 0.1 s release. */
    h5_env_set_times(&sn->env, H5_SNARE_ATTACK_Q16, decay, sample_rate);
    h5_env_set_times(&sn->env_click, H5_SNARE_CLICK_ATTACK_Q16,
        (2 * decay + H5_Q16_ONE) / 10, sample_rate);

    /* Rattle resonators and output low-pass are tuned on parameter change. */
    h5_svf_tune(&sn->rattle[0], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_1,
        H5_SNARE_RATTLE_Q16, sample_rate);
    h5_svf_tune(&sn->rattle[1], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_2,
        H5_SNARE_RATTLE_Q16, sample_rate);
    h5_svf_tune(&sn->rattle[2], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_3,
        H5_SNARE_RATTLE_Q16, sample_rate);
    h5_svf_tune(&sn->rattle[3], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_4,
        H5_SNARE_RATTLE_Q16, sample_rate);
    h5_svf_tune(&sn->out_filter, H5_SVF_LP, params[H5_PARAM_NOISE_PITCH],
        H5_SNARE_OUT_Q16, sample_rate);
}

void h5_snare_trigger(struct h5_snare_s *sn)
{
    size_t i;

    sn->phase = 0u;
    h5_env_trigger(&sn->env);
    h5_env_trigger(&sn->env_click);
    for (i = 0u; i < 4u; i++) {
        h5_svf_reset(&sn->rattle[i]);
    }
    h5_svf_reset(&sn->out_filter);
    sn->active = 1u;
}

int32_t h5_snare_process(struct h5_snare_s *sn, uint32_t sample_rate)
{
    int32_t env, env_click, env3, semitones_q16, ratio, inc, s, s2, u;
    int32_t tone, rattle, sum, body, click, mix, half, blended;
    int32_t sweep, cutoff, y, out;
    size_t i;

    if (!sn->active) {
        return 0;
    }

    env = h5_env_next(&sn->env);
    env_click = h5_env_next(&sn->env_click);
    if (env <= 0 && env_click <= 0
        && sn->env.state == H5_ENV_DONE
        && sn->env_click.state == H5_ENV_DONE) {
        sn->active = 0u;
        return 0;
    }

    env3 = h5_cube_q30(env);

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env / 12). */
    semitones_q16 = (int32_t)h5_shr_round((int64_t)sn->bend_q16 * env, 30u);
    semitones_q16 = (int32_t)h5_shr_round(
        (int64_t)semitones_q16 * H5_BEND_LOG2_3_2_Q16, 16u);
    ratio = h5_semitone_ratio_interp(semitones_q16);
    inc = (int32_t)h5_shr_round(h5_mulwide(sn->base_inc, ratio), 16u);
    if (inc < 1) {
        inc = 1;
    }
    sn->phase = (sn->phase + (uint32_t)inc) & 0x0FFFFFFFu;

    s = h5_sine_interp(sn->phase);
    /* s2 = sin((pi/2) * s); a quarter of s's normalized value is 2^26 of a
     * 28-bit phase, i.e. s >> 5 for a Q31 sample. */
    s2 = h5_sine_interp((uint32_t)(s >> 5));

    /* Four band-pass filters on the held noise. */
    u = h5_noise_next(&sn->noise);
    rattle = 0;
    for (i = 0u; i < 4u; i++) {
        rattle = h5_sat_add_q31(rattle, h5_svf_run(&sn->rattle[i], u));
    }

    /* Body: env^3 * (1-Click)^0.7
     *       * (NoiseTone^0.7 * s + 0.3*(1-NoiseTone)^0.7 * rattle) */
    tone = h5_mul_q16(s, sn->tone_gain_q16);
    sum = h5_sat_add_q31(tone,
        h5_mul_q16(rattle, sn->noise_gain_q16));
    body = h5_mul_q30(sum, env3);
    body = h5_mul_q16(body, sn->body_gain_q16);

    /* Transient: env_click^10 * Click^0.7 * rattle. */
    click = h5_mul_q16(rattle, sn->click_gain_q16);
    click = h5_mul_q30(click, h5_pow10_q30(env_click));
    mix = h5_sat_add_q31(body, click);

    /* Sine-shaper branch: 0.5*(s2 + s), scaled by the body envelope and
     * click law. The Drive control blends it into the mix before tanh. */
    half = h5_mul_q30((int32_t)(((int64_t)s2 + (int64_t)s) / 2), env3);
    half = h5_mul_q16(half, sn->body_gain_q16);
    blended = h5_sat_add_q31(
        h5_mul_q16(mix, H5_Q16_ONE - H5_SNARE_DRIVE_Q16),
        h5_mul_q16(half, H5_SNARE_DRIVE_Q16));

    /* Sweep lowers the output cutoff as the body decays. The contour depth
     * is half the base cutoff at full Sweep. */
    sweep = h5_mul_q16(H5_SNARE_SWEEP_DEPTH_Q16, sn->sweep_q16);
    cutoff = h5_mul_q16(sn->noise_pitch_q16,
        H5_Q16_ONE - h5_mul_q16(sweep, H5_Q16_ONE - (env3 >> 14)));
    h5_svf_tune(&sn->out_filter, H5_SVF_LP, cutoff, H5_SNARE_OUT_Q16,
        sample_rate);

    y = h5_softclip_q31(blended);

    out = h5_svf_run(&sn->out_filter, y);
    out = h5_mul_q16(out, H5_SNARE_VOLUME_Q16);
    /* Previous operation: out = h5_mul_q16(out, H5_SNARE_GAIN_Q16);
     * The actual formula is out = volume * gain. Gain is fixed at 1.0, so
     * this multiply is an identity and is elided. Restore it if the gain
     * value ever changes. */

    return out;
}
