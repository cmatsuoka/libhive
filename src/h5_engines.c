/*
 * h5_engines.c - per-voice DSP engines.
 *
 * Every tone generator follows one architecture: a tonal branch (a sine
 * oscillator with a drive blend into a shaped sine branch), a sustained
 * noise branch (filtered noise under its own AR envelope) and a short
 * transient burst on the filtered noise. All three branches sum into a
 * soft-clip shaper and leave through a fixed output filter.
 *
 *   - Tone path: the tone envelope is a linear AR whose value is cubed;
 *     the cube drives both the tone amplitude and the pitch exponent.
 *     The tone branch pairs a sine oscillator with a shaped sine
 *     (0.5*(s2+s)) through the Drive blend; this approximates the
 *     original device's filtered triangle core. The Kick tone attack
 *     is 0.005*Decay and Drive blends the sine with the shaped-sine
 *     branch whose weight per engine differs (Kick 4.7, Tom/Snare 1.0);
 *     the shaper branch follows the body envelope so it decays with the
 *     sound.
 *   - Sustained noise: filtered noise * AR(noise_attack, NoiseDecay) *
 *     noiseWeight, scaling linearly with trigger gain.
 *   - Transient burst: AR^10 over the same filtered noise with a 1 ms
 *     attack, weighted Click^0.7 (velocity factors evaluate to 1.0 at
 *     full gain; velocity handling lives in the trigger layer).
 *   - Pitch envelope: f = Pitch * 1.5^(Bend*env^3/12) applies to the
 *     tonal branch of each voice.
 *   - Noise cutoff sweep: (NoisePitch + 2000*env_n^3)
 *     * (1 - FilterBend*(1 - env_n^3)); FilterBend 0.5 on the Kick and
 *     0.55/0.57/0.56 per Tom instance. The noise passes a fixed 50 Hz
 *     high-pass / 20 kHz low-pass band limit first.
 *   - Output: 1.5 * tanh(mix) * 1.10479 * output filter. The engine emits
 *     Q30 (1.0 = 1 << 30) with one headroom bit; the mixer applies the
 *     user Volume exactly once.
 *
 * Drive, Resonance and Gain are engine constants (0.13, 1.0, 1.0), not
 * exposed parameters. Each trigger restarts the oscillator phase, the
 * envelopes and the filters; the noise generator free-runs.
 */

#include "h5_internal.h"

/* log2(1.5) in Q16.16. Scaling the bend depth by this converts the engine's
 * 1.5^(n/12) curve into 2^(n*log2(1.5)/12), which the semitone table covers. */
#define H5_BEND_LOG2_3_2_Q16 (38336)

/* Fixed output gain after the shaper: 1.10479. */
#define H5_OUT_GAIN_Q16 (72404)

/* One-pole fixed cutoffs, Hz in Q16: the noise band limit (50 Hz high-pass,
 * 20 kHz low-pass) and a dark 338.6 Hz low-pass that tones down the Kick's
 * transient burst. */
#define H5_PF_LO_HZ (50 * H5_Q16_ONE)
#define H5_PF_HI_HZ (20000 * H5_Q16_ONE)
#define H5_PF_338_HZ (22190489) /* 338.6 Hz in Q16 */

/* Cutoff sweep contribution at env = 1, Hz in Q16 (velocity depth factors
 * fold to 1.0 at full gain; velocity handling lives in the trigger layer). */
#define H5_NOISE_SWEEP_Q16 (2000 * H5_Q16_ONE)

/* Kick path weights and constants, Q16. */
#define H5_KICK_TONE_WEIGHT_Q16 (9 * H5_Q16_ONE / 10)  /* 0.9 */
#define H5_KICK_NOISE_WEIGHT_Q16 (3 * H5_Q16_ONE / 10) /* 0.3 */
#define H5_KICK_CLICK_WEIGHT_Q16 (9 * H5_Q16_ONE / 10) /* 0.9 */
#define H5_KICK_DRIVE_Q16 (13 * H5_Q16_ONE / 100)
#define H5_KICK_SHAPER_WEIGHT_Q16 (47 * H5_Q16_ONE / 10)
#define H5_KICK_FILTER_BEND_Q16 (H5_Q16_ONE / 2)
#define H5_KICK_RESONANCE_Q16 H5_Q16_ONE
#define H5_KICK_OUT_HP_HZ 15
#define H5_KICK_OUT_Q16 (46341) /* 1/sqrt(2) */

/* Tom path constants, Q16. The transient click scale folds the velocity
 * factor at full gain. */
#define H5_TOM_DRIVE_Q16 (13 * H5_Q16_ONE / 100)
#define H5_TOM_SHAPER_WEIGHT_Q16 H5_Q16_ONE
#define H5_TOM_RESONANCE_Q16 H5_Q16_ONE
#define H5_TOM_OUT_HP_HZ 25
#define H5_TOM_OUT_Q16 (46341) /* 1/sqrt(2) */
#define H5_TOM_NOISE_WEIGHT_Q16 (3 * H5_Q16_ONE / 10)
#define H5_TOM_CLICK_GAIN_Q16 H5_Q16_ONE
#define H5_TOM_FILTER_BEND_1_Q16 (55 * H5_Q16_ONE / 100)
#define H5_TOM_FILTER_BEND_2_Q16 (57 * H5_Q16_ONE / 100)
#define H5_TOM_FILTER_BEND_3_Q16 (56 * H5_Q16_ONE / 100)

/* Snare path constants, Q16. */
#define H5_SNARE_DRIVE_Q16 (13 * H5_Q16_ONE / 100)
#define H5_SNARE_SHAPER_WEIGHT_Q16 H5_Q16_ONE
#define H5_SNARE_OUT_Q16 H5_Q16_ONE
#define H5_SNARE_ATTACK_Q16 H5_MS_Q16(1)
#define H5_SNARE_CLICK_ATTACK_Q16 H5_MS_Q16(1)
#define H5_SNARE_NOISE_WEIGHT_Q16 (3 * H5_Q16_ONE / 10)
#define H5_SNARE_SWEEP_DEPTH_Q16 (H5_Q16_ONE / 2)

/* Rattle band-pass bank: two narrow wooden resonances around 330 and
 * 600 Hz, a wide low-mid body band near 220 Hz and a broad 2.6 kHz shelf,
 * with per-band gain weights that shape the sum. */
#define H5_SNARE_RATTLE_FREQ_1 (335 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_2 (596 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_3 (220 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_4 (2600 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_Q_1 (4 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_Q_2 (4 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_Q_3 (27 * H5_Q16_ONE / 20)
#define H5_SNARE_RATTLE_Q_4 (2 * H5_Q16_ONE / 5)
#define H5_SNARE_RATTLE_WEIGHT_1 (110904)
#define H5_SNARE_RATTLE_WEIGHT_2 (240624)
#define H5_SNARE_RATTLE_WEIGHT_3 (85368)
#define H5_SNARE_RATTLE_WEIGHT_4 (31284)

/* Milliseconds to Q16 seconds (round-half-up). */
#define H5_MS_Q16(ms) (((ms) * H5_Q16_ONE + 500) / 1000)

/* Q30 unity: the one-pole coefficient that passes the input through. */
#define H5_PF_PASS_Q30 (1 << 30)

/* The one-pole coefficient k = tan/(1+tan) in Q30. A cutoff at or above
 * Nyquist is bypassed: tan(pi*f/sr) is zero at sr/2 and negative beyond it,
 * so the analogue prototype has no usable coefficient there. h5_svf_tune()
 * guards its swept filters the same way by clamping their cutoffs below
 * Nyquist. */
static int32_t h5_pf_k(int32_t f_q16, uint32_t sample_rate)
{
    int32_t t;
    int64_t den, q;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }
    if (f_q16 <= 0
        || (uint64_t)(uint32_t)f_q16 * 2u
            >= (uint64_t)sample_rate * H5_Q16_ONE) {
        return H5_PF_PASS_Q30;
    }
    t = h5_tanpi_q16(f_q16, sample_rate);
    den = (int64_t)H5_Q16_ONE + t;
    if (t <= 0 || den <= 0) {
        return H5_PF_PASS_Q30;
    }
    q = ((int64_t)t << 30) / den;
    if (q >= H5_PF_PASS_Q30) {
        return H5_PF_PASS_Q30;
    }
    return (int32_t)q;
}

/* The fixed 50 Hz high-pass / 20 kHz low-pass band limit on the held noise. */
static int32_t h5_prefilter(int32_t *l50, int32_t *l20, int32_t k50,
    int32_t k20, int32_t x)
{
    int32_t hp, lp;

    lp = h5_sat_add_q31(*l50, h5_mul_q30(h5_sat_sub_q31(x, *l50), k50));
    *l50 = lp;
    hp = h5_sat_sub_q31(x, lp);
    lp = h5_sat_add_q31(*l20, h5_mul_q30(h5_sat_sub_q31(hp, *l20), k20));
    *l20 = lp;
    return lp;
}

/* Common fixed one-pole setup for one sample rate. k338 is filled only
 * for voices that tone down their transient burst (pass NULL elsewhere). */
static void h5_pf_setup(int32_t *k50, int32_t *k20, int32_t *k338,
    uint32_t sample_rate)
{
    *k50 = h5_pf_k(H5_PF_LO_HZ, sample_rate);
    *k20 = h5_pf_k(H5_PF_HI_HZ, sample_rate);
    if (k338) {
        *k338 = h5_pf_k(H5_PF_338_HZ, sample_rate);
    }
}

/* x^10 in Q30: (v^8 * v^2), three squarings. */
static int32_t h5_pow10_q30(int32_t v)
{
    int32_t v2 = (int32_t)h5_shr_round((int64_t)v * v, 30u);
    int32_t v4 = (int32_t)h5_shr_round((int64_t)v2 * v2, 30u);
    int32_t v8 = (int32_t)h5_shr_round((int64_t)v4 * v4, 30u);

    return (int32_t)h5_shr_round((int64_t)v8 * v2, 30u);
}

/* Cutoff laws for the three swept filters, in Hz Q16. env16 is the cubed
 * envelope as Q16 (0..H5_Q16_ONE). These are called once per sample with the
 * current envelope and, for the table builders, at the two endpoints. */
static int32_t h5_sweep_cutoff(int32_t noise_pitch_q16, int32_t bend_q16,
    int32_t env16)
{
    int32_t base = h5_sat_i32((int64_t)noise_pitch_q16
        + h5_mul_q16(H5_NOISE_SWEEP_Q16, env16));

    return h5_sat_i32((int64_t)base
        * (int64_t)(H5_Q16_ONE
            - h5_mul_q16(bend_q16, H5_Q16_ONE - env16))
        >> 16);
}

static int32_t h5_snare_cutoff(int32_t noise_pitch_q16, int32_t sweep_q16,
    int32_t env16)
{
    int32_t sweep = h5_mul_q16(H5_SNARE_SWEEP_DEPTH_Q16, sweep_q16);

    return h5_mul_q16(noise_pitch_q16,
        H5_Q16_ONE - h5_mul_q16(sweep, H5_Q16_ONE - env16));
}

/* Oscillator increment for f = TonePitch * 1.5^(Bend*env^3/12). env3 is the
 * cubed Q30 envelope; the product with the Q16 bend depth is scaled by
 * log2(1.5) and looked up in the semitone table, which realizes 2^(n/12). */
static int32_t h5_pitch_inc(int32_t base_inc, int32_t bend_q16, int32_t env3)
{
    int32_t semitones_q16, ratio;

    semitones_q16 = (int32_t)h5_shr_round((int64_t)bend_q16 * env3, 30u);
    semitones_q16 = (int32_t)h5_shr_round(
        (int64_t)semitones_q16 * H5_BEND_LOG2_3_2_Q16, 16u);
    ratio = h5_semitone_ratio_interp(semitones_q16);
    return (int32_t)h5_shr_round(h5_mulwide(base_inc, ratio), 16u);
}

/* Per-band rattle weights (with the folded output scale), Q16. */
static const int32_t h5_snare_rattle_weight[4] = {
    H5_SNARE_RATTLE_WEIGHT_1, H5_SNARE_RATTLE_WEIGHT_2,
    H5_SNARE_RATTLE_WEIGHT_3, H5_SNARE_RATTLE_WEIGHT_4
};

/*
 * Kick engine.
 *
 * Signal flow:
 *   env_n  = AR(~2 ms, NoiseDecay)
 *   env_t  = AR(0.005*Decay, Decay), used cubed
 *   env_c  = AR^10(1 ms, 0.2*Decay+0.1)
 *   fc_n   = (NoisePitch + 2000*env_n^3) * (1 - 0.5*(1 - env_n^3))
 *   nf     = SVF_LP(bandlimit(u_hold); fc_n, Resonance)
 *   noise  = nf * env_n * 0.3*(1-NoiseTone)^0.7 * (1-Click)^0.7
 *   click  = LP338(nf) * env_c^10 * 0.9*Click^0.7
 *   f      = TonePitch * 1.5^(Bend*env_t^3/12)
 *   s      = sin(2*pi*phi), s2 = sin((pi/2)*s)
 *   tone   = env_t^3 * 0.9*NoiseTone^0.7*(1-Click)^0.7
 *            * ((1-Drive)*s + Drive*4.7*0.5*(s2+s))
 *   out    = 1.5*tanh(tone+noise+click) * 1.10479 -> HP15
 *   (the single Decay control writes both NoiseDecay and ToneDecay)
 */

void h5_kick_init(struct h5_kick_s *k, uint32_t sample_rate)
{
    h5_env_init(&k->env_noise);
    h5_env_init(&k->env_tone);
    h5_env_init(&k->env_click);
    h5_noise_init_voice(&k->noise, H5_VOICE_KICK, sample_rate);
    h5_svf_reset(&k->noise_filter);
    h5_svf_reset(&k->out_filter);
    k->phase = 0u;
    k->base_inc = 0;
    k->bend_q16 = 0;
    k->noise_pitch_q16 = 0;
    k->tone_gain_q16 = 0;
    k->noise_gain_q16 = 0;
    k->click_gain_q16 = 0;
    k->pf_k50 = 0;
    k->pf_k20 = 0;
    k->pf_l50 = 0;
    k->pf_l20 = 0;
    k->pf_k338 = 0;
    k->pf_l338 = 0;
    k->active = 0u;
}

void h5_kick_configure(struct h5_kick_s *k,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    int64_t inc;
    int32_t click, decay, noise_attack;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    k->bend_q16 = params[H5_PARAM_BEND];
    k->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];

    /* NoiseTone is the tone amount; Click splits tone and sustain noise
     * through mirrored x^0.7 curves. */
    k->tone_gain_q16 = h5_mul_q16(H5_KICK_TONE_WEIGHT_Q16,
        h5_mul_q16(h5_pow07_q16(params[H5_PARAM_NOISE_TONE]),
            h5_pow07_q16(H5_Q16_ONE - click)));
    k->noise_gain_q16 = h5_mul_q16(H5_KICK_NOISE_WEIGHT_Q16,
        h5_mul_q16(h5_pow07_q16(H5_Q16_ONE - params[H5_PARAM_NOISE_TONE]),
            h5_pow07_q16(H5_Q16_ONE - click)));
    k->click_gain_q16 = h5_mul_q16(H5_KICK_CLICK_WEIGHT_Q16,
        h5_pow07_q16(click));

    /* cycles/sample = pitch/rate; the 28-bit phase unit adds 2^28. pitch is
     * Q16 Hz, so the numerator is pitch << 12. */
    inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;
    if (inc < 1) {
        inc = 1;
    }
    k->base_inc = (int32_t)inc;

    /* Envelopes. The tone env drives both amplitude and pitch and starts
     * fast; the noise env shares the tone release; the click burst keeps
     * a ~1 ms attack with its own 0.2*NoiseDecay+0.1 release. */
    h5_env_set_times(&k->env_tone, decay / 200, decay, sample_rate);
    noise_attack = (2 * decay) / 10 + H5_Q16_ONE / 10;
    h5_env_set_times(&k->env_noise, H5_MS_Q16(2), decay, sample_rate);
    h5_env_set_times(&k->env_click, H5_MS_Q16(1), noise_attack, sample_rate);

    h5_pf_setup(&k->pf_k50, &k->pf_k20, &k->pf_k338, sample_rate);
    h5_svf_tune(&k->out_filter, H5_SVF_HP,
        H5_KICK_OUT_HP_HZ * H5_Q16_ONE, H5_KICK_OUT_Q16, sample_rate);
    h5_sweep_build(&k->noise_sweep, H5_SVF_LP, H5_KICK_RESONANCE_Q16,
        h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16, 0),
        h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16,
            H5_Q16_ONE),
        sample_rate);
}

void h5_kick_trigger(struct h5_kick_s *k)
{
    k->phase = 0u;
    h5_env_trigger(&k->env_noise);
    h5_env_trigger(&k->env_tone);
    h5_env_trigger(&k->env_click);
    h5_svf_reset(&k->noise_filter);
    h5_svf_reset(&k->out_filter);
    k->pf_l50 = 0;
    k->pf_l20 = 0;
    k->pf_l338 = 0;
    k->active = 1u;
}

int32_t h5_kick_process(struct h5_kick_s *k)
{
    int32_t env_n, env_t, env_c, tone3, env3;
    int32_t inc, s, s2, cutoff, u, nf, noise, trans, tone, sum;
    int32_t blend, out;

    if (!k->active) {
        return 0;
    }

    env_n = h5_env_next(&k->env_noise);
    env_c = h5_env_next(&k->env_click);
    env_t = h5_env_next(&k->env_tone);
    if (env_n <= 0 && env_t <= 0 && env_c <= 0
        && k->env_noise.state == H5_ENV_DONE
        && k->env_tone.state == H5_ENV_DONE
        && k->env_click.state == H5_ENV_DONE) {
        k->active = 0u;
        return 0;
    }

    /* The tone envelope value, cubed once; the cube drives both the tone
     * amplitude and the pitch exponent below. */
    tone3 = h5_cube_q30(env_t);

    /* Pitch: f = TonePitch * 1.5^(Bend * tone3 / 12). */
    inc = h5_pitch_inc(k->base_inc, k->bend_q16, tone3);
    if (inc < 1) {
        inc = 1;
    }
    k->phase = (k->phase + (uint32_t)inc) & 0x0FFFFFFFu;

    s = h5_sine_interp(k->phase);
    s2 = h5_sine_interp((uint32_t)(s >> 5));

    /* Noise band limit then swept low-pass:
     * fc_n = (NoisePitch + 2000*env_n^3) * (1 - 0.5*(1 - env_n^3)). */
    env3 = h5_cube_q30(env_n);
    cutoff = h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16,
        env3 >> 14);
    h5_sweep_apply(&k->noise_sweep, cutoff, &k->noise_filter);
    u = h5_prefilter(&k->pf_l50, &k->pf_l20, k->pf_k50, k->pf_k20,
        h5_noise_next(&k->noise));
    nf = h5_svf_run(&k->noise_filter, u);

    /* Sustained noise: nf * env_n * 0.3*(1-NoiseTone)^0.7*(1-Click)^0.7. */
    noise = h5_mul_q30(nf, env_n);
    noise = h5_mul_q16(noise, k->noise_gain_q16);

    /* Transient burst: LP338(nf) * env_c^10 * 0.9*Click^0.7. The burst
     * carries one headroom bit past the sustained branches; the fixed
     * low-pass keeps its energy centered below the mids. The branch is
     * silent once the burst envelope has decayed, so skip its work. */
    trans = 0;
    if (env_c > 0) {
        nf = h5_sat_add_q31(k->pf_l338, h5_mul_q30(h5_sat_sub_q31(nf,
            k->pf_l338), k->pf_k338));
        k->pf_l338 = nf;
        trans = h5_mul_q16(nf, k->click_gain_q16);
        trans = h5_mul_q30(trans, h5_pow10_q30(env_c));
    }

    /* Tone: env^3 * 0.9*NoiseTone^0.7*(1-Click)^0.7
     *       * ((1-Drive)*s + Drive*4.7*0.5*(s2+s)). */
    blend = h5_sat_add_q31(
        h5_mul_q16(s, H5_Q16_ONE - H5_KICK_DRIVE_Q16),
        h5_mul_q16((int32_t)(((int64_t)s2 + (int64_t)s) / 2),
            h5_mul_q16(H5_KICK_DRIVE_Q16, H5_KICK_SHAPER_WEIGHT_Q16)));
    tone = h5_mul_q30(blend, tone3);
    tone = h5_mul_q16(tone, k->tone_gain_q16);

    sum = h5_sat_add_q31(tone, noise);
    sum = h5_sat_add_q31(sum, trans);
    out = h5_softclip_q31(sum);

    /* Fixed output gain 1.10479 into the 15 Hz high-pass. */
    out = h5_mul_q16(out, H5_OUT_GAIN_Q16);
    out = h5_svf_run(&k->out_filter, out);
    /* User volume is applied once, by the mixer. */
    return out;
}

/*
 * Tom engine.
 *
 * Signal flow:
 *   env     = AR^3(0.005*Decay, Decay)
 *   env_c   = AR^10(1 ms, 0.25*Decay)
 *   fc_n    = (NoisePitch + 2000*env^3) * (1 - FB*(1 - env^3))
 *   nf      = SVF_LP(bandlimit(u_hold); fc_n, Resonance)
 *   body    = env^3 * (1-Click)^0.7 * (NoiseTone^0.7*s
 *                                          + 0.3*(1-NoiseTone)^0.7*nf)
 *   click   = env_c^10 * Click^0.7 * nf
 *   out     = 1.5*tanh((1-Drive)*mix + Drive*0.5*(s2+s)) * 1.10479 -> HP25
 *   (each Tom instance tunes its own filter-bend depth)
 */

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
    t->filter_bend_q16 = H5_TOM_FILTER_BEND_1_Q16;
    t->tone_gain_q16 = 0;
    t->noise_gain_q16 = 0;
    t->body_gain_q16 = 0;
    t->click_gain_q16 = 0;
    t->pf_k50 = 0;
    t->pf_k20 = 0;
    t->pf_l50 = 0;
    t->pf_l20 = 0;
    t->active = 0u;
    if (voice == H5_VOICE_TOM_2) {
        t->filter_bend_q16 = H5_TOM_FILTER_BEND_2_Q16;
    } else if (voice == H5_VOICE_TOM_3) {
        t->filter_bend_q16 = H5_TOM_FILTER_BEND_3_Q16;
    }
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

    t->tone_gain_q16 = h5_pow07_q16(noise_tone);
    t->noise_gain_q16 = h5_mul_q16(
        h5_pow07_q16(H5_Q16_ONE - noise_tone), H5_TOM_NOISE_WEIGHT_Q16);
    t->body_gain_q16 = h5_pow07_q16(H5_Q16_ONE - click);
    t->click_gain_q16 = h5_mul_q16(h5_pow07_q16(click),
        H5_TOM_CLICK_GAIN_Q16);

    inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;
    if (inc < 1) {
        inc = 1;
    }
    t->base_inc = (int32_t)inc;

    /* Body envelope: AR^3 with a 0.005*Decay attack and the Decay release.
     * Transient envelope: AR^10, 1 ms attack, 0.25*Decay release. */
    h5_env_set_times(&t->env, decay / 200, decay, sample_rate);
    h5_env_set_times(&t->env_click, H5_MS_Q16(1), decay / 4, sample_rate);

    h5_pf_setup(&t->pf_k50, &t->pf_k20, 0, sample_rate);
    h5_svf_tune(&t->out_filter, H5_SVF_HP,
        H5_TOM_OUT_HP_HZ * H5_Q16_ONE, H5_TOM_OUT_Q16, sample_rate);
    h5_sweep_build(&t->noise_sweep, H5_SVF_LP, H5_TOM_RESONANCE_Q16,
        h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16, 0),
        h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16, H5_Q16_ONE),
        sample_rate);
}

void h5_tom_trigger(struct h5_tom_s *t)
{
    t->phase = 0u;
    h5_env_trigger(&t->env);
    h5_env_trigger(&t->env_click);
    h5_svf_reset(&t->noise_filter);
    h5_svf_reset(&t->out_filter);
    t->pf_l50 = 0;
    t->pf_l20 = 0;
    t->active = 1u;
}

int32_t h5_tom_process(struct h5_tom_s *t)
{
    int32_t env, env_click, env3;
    int32_t inc, s, s2, cutoff, u, nf, tone, noise, sum;
    int32_t body, click, mix, half, blended, out;

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

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env^3 / 12). */
    inc = h5_pitch_inc(t->base_inc, t->bend_q16, env3);
    if (inc < 1) {
        inc = 1;
    }
    t->phase = (t->phase + (uint32_t)inc) & 0x0FFFFFFFu;

    s = h5_sine_interp(t->phase);
    s2 = h5_sine_interp((uint32_t)(s >> 5));

    /* fc_n = (NoisePitch + 2000*env^3)*(1 - FB*(1 - env^3)). */
    cutoff = h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16,
        env3 >> 14);
    h5_sweep_apply(&t->noise_sweep, cutoff, &t->noise_filter);
    u = h5_prefilter(&t->pf_l50, &t->pf_l20, t->pf_k50, t->pf_k20, h5_noise_next(&t->noise));
    nf = h5_svf_run(&t->noise_filter, u);

    /* Body: env^3 * (1-Click)^0.7 * (NoiseTone^0.7*s + 0.3*(1-NoiseTone)^0.7*noise). */
    tone = h5_mul_q16(s, t->tone_gain_q16);
    noise = h5_mul_q16(nf, t->noise_gain_q16);
    sum = h5_sat_add_q31(tone, noise);
    body = h5_mul_q30(sum, env3);
    body = h5_mul_q16(body, t->body_gain_q16);

    /* Transient burst: env_c^10 * Click^0.7 * nf (velocity factors evaluate
     * to 1.0 at full gain). Skip it once the burst envelope is silent. */
    click = 0;
    if (env_click > 0) {
        click = h5_mul_q16(nf, t->click_gain_q16);
        click = h5_mul_q30(click, h5_pow10_q30(env_click));
    }
    mix = h5_sat_add_q31(body, click);

    /* The sine-shaper branch rides the body envelope so it decays with
     * the sound instead of ringing on. */
    half = h5_mul_q30((int32_t)(((int64_t)s2 + (int64_t)s) / 2), env3);
    half = h5_mul_q16(half, t->body_gain_q16);
    blended = h5_sat_add_q31(
        h5_mul_q16(mix, H5_Q16_ONE - H5_TOM_DRIVE_Q16),
        h5_mul_q16(half, h5_mul_q16(H5_TOM_DRIVE_Q16,
        H5_TOM_SHAPER_WEIGHT_Q16)));
    out = h5_softclip_q31(blended);

    out = h5_mul_q16(out, H5_OUT_GAIN_Q16);
    out = h5_svf_run(&t->out_filter, out);
    return out;
}

/*
 * Snare engine.
 *
 * Signal flow:
 *   env     = AR^3(1 ms, Decay)
 *   env_c   = AR^10(1 ms, 0.2*Decay+0.1)
 *   rattle  = 4 weighted band-passes over the band-limited held noise
 *   body    = env^3 * (1-Click)^0.7 * (NoiseTone^0.7*s
 *                                          + 0.3*(1-NoiseTone)^0.7*rattle)
 *   click   = env_c^10 * Click^0.7 * rattle
 *   out     = 1.5*tanh(blend) * 1.10479 -> LP at NoisePitch*Sweep contour
 */

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
    sn->pf_k50 = 0;
    sn->pf_k20 = 0;
    sn->pf_l50 = 0;
    sn->pf_l20 = 0;
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

    sn->tone_gain_q16 = h5_pow07_q16(noise_tone);
    sn->noise_gain_q16 = h5_mul_q16(
        h5_pow07_q16(H5_Q16_ONE - noise_tone), H5_SNARE_NOISE_WEIGHT_Q16);
    sn->body_gain_q16 = h5_pow07_q16(H5_Q16_ONE - click);
    sn->click_gain_q16 = h5_pow07_q16(click);

    inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;
    if (inc < 1) {
        inc = 1;
    }
    sn->base_inc = (int32_t)inc;

    /* Body envelope: AR^3, 1 ms attack, Decay release. Transient: AR^10,
     * 1 ms attack, 0.2*Decay + 0.1 s release. */
    h5_env_set_times(&sn->env, H5_SNARE_ATTACK_Q16, decay, sample_rate);
    h5_env_set_times(&sn->env_click, H5_SNARE_CLICK_ATTACK_Q16,
        (2 * decay + H5_Q16_ONE) / 10, sample_rate);

    /* Rattle resonators, output low-pass, pre-filter: parameter change. */
    h5_pf_setup(&sn->pf_k50, &sn->pf_k20, 0, sample_rate);
    h5_svf_tune(&sn->rattle[0], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_1,
        H5_SNARE_RATTLE_Q_1, sample_rate);
    h5_svf_tune(&sn->rattle[1], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_2,
        H5_SNARE_RATTLE_Q_2, sample_rate);
    h5_svf_tune(&sn->rattle[2], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_3,
        H5_SNARE_RATTLE_Q_3, sample_rate);
    h5_svf_tune(&sn->rattle[3], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_4,
        H5_SNARE_RATTLE_Q_4, sample_rate);
    h5_svf_tune(&sn->out_filter, H5_SVF_LP, params[H5_PARAM_NOISE_PITCH],
        H5_SNARE_OUT_Q16, sample_rate);
    h5_sweep_build(&sn->out_sweep, H5_SVF_LP, H5_SNARE_OUT_Q16,
        h5_snare_cutoff(sn->noise_pitch_q16, sn->sweep_q16, 0),
        h5_snare_cutoff(sn->noise_pitch_q16, sn->sweep_q16, H5_Q16_ONE),
        sample_rate);
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
    sn->pf_l50 = 0;
    sn->pf_l20 = 0;
    sn->active = 1u;
}

int32_t h5_snare_process(struct h5_snare_s *sn)
{
    int32_t env, env_click, env3, inc, s, s2, u;
    int32_t tone, rattle, sum, body, click, mix, half, blended;
    int32_t cutoff, out;
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

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env^3 / 12). */
    inc = h5_pitch_inc(sn->base_inc, sn->bend_q16, env3);
    if (inc < 1) {
        inc = 1;
    }
    sn->phase = (sn->phase + (uint32_t)inc) & 0x0FFFFFFFu;

    s = h5_sine_interp(sn->phase);
    s2 = h5_sine_interp((uint32_t)(s >> 5));

    /* Four weighted band-pass filters on the band-limited held noise. */
    u = h5_prefilter(&sn->pf_l50, &sn->pf_l20, sn->pf_k50, sn->pf_k20,
        h5_noise_next(&sn->noise));
    rattle = 0;
    for (i = 0u; i < 4u; i++) {
        rattle = h5_sat_add_q31(rattle, h5_mul_q16(
            h5_svf_run(&sn->rattle[i], u), h5_snare_rattle_weight[i]));
    }

    /* Body: env^3 * (1-Click)^0.7
     *       * (NoiseTone^0.7*s + 0.3*(1-NoiseTone)^0.7*rattle). */
    tone = h5_mul_q16(s, sn->tone_gain_q16);
    sum = h5_sat_add_q31(tone, h5_mul_q16(rattle, sn->noise_gain_q16));
    body = h5_mul_q30(sum, env3);
    body = h5_mul_q16(body, sn->body_gain_q16);

    /* Transient: env_c^10 * Click^0.7 * rattle (velocity factors evaluate
     * to 1.0 at full gain). Skip it once the burst envelope is silent. */
    click = 0;
    if (env_click > 0) {
        click = h5_mul_q16(rattle, sn->click_gain_q16);
        click = h5_mul_q30(click, h5_pow10_q30(env_click));
    }
    mix = h5_sat_add_q31(body, click);

    /* The shaper branch rides the body envelope (see Tom). */
    half = h5_mul_q30((int32_t)(((int64_t)s2 + (int64_t)s) / 2), env3);
    half = h5_mul_q16(half, sn->body_gain_q16);
    blended = h5_sat_add_q31(
        h5_mul_q16(mix, H5_Q16_ONE - H5_SNARE_DRIVE_Q16),
        h5_mul_q16(half, h5_mul_q16(H5_SNARE_DRIVE_Q16,
            H5_SNARE_SHAPER_WEIGHT_Q16)));

    /* Sweep lowers the output cutoff as the body decays. The contour depth
     * is half the base cutoff at full Sweep. */
    cutoff = h5_snare_cutoff(sn->noise_pitch_q16, sn->sweep_q16, env3 >> 14);
    h5_sweep_apply(&sn->out_sweep, cutoff, &sn->out_filter);

    out = h5_softclip_q31(blended);

    out = h5_mul_q16(out, H5_OUT_GAIN_Q16);
    out = h5_svf_run(&sn->out_filter, out);
    return out;
}
