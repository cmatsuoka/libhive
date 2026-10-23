/*
 * h5_param.c - public parameter tables, validation and getters/setters.
 *
 * The h5_param_table is the single source of truth for which parameter is
 * valid for which voice, and for the default values and clamping ranges.
 * Values are stored as Q16.16 physical units: Hz, seconds, signed semitones,
 * or a 0..1 scalar.
 */

#include "h5_internal.h"

#define Q H5_Q16_ONE

/* Helper for integer physical values. */
#define I(v) ((int32_t)(v) * Q)

/* Helper for fractional physical values using round-half-up. */
#define F(num, den) ((int32_t)(((int64_t)(num) * Q + ((den) / 2u)) / (den)))

const struct h5_param_info h5_param_table[H5_VOICE_COUNT][H5_PARAM_COUNT] = {
    [H5_VOICE_KICK] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(11000), true },
        [H5_PARAM_TONE_PITCH]  = { I(10),  I(440),   I(20),    true },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(48),    true },
        [H5_PARAM_DECAY]       = { F(1, 10), F(2, 1), F(4, 10), true },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(9, 10), true },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(1, 2),  true },
        [H5_PARAM_SWEEP]       = { 0,      0,        0,        false },
        [H5_PARAM_BELL_CYMBAL] = { 0,      0,        0,        false },
    },
    [H5_VOICE_SNARE] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(14000), I(10000), true },
        [H5_PARAM_TONE_PITCH]  = { I(28),  I(880),   I(110),   true },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(20),    true },
        [H5_PARAM_DECAY]       = { F(2, 10), F(5, 1), F(1, 1), true },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(7, 10), true },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(7, 10), true },
        [H5_PARAM_SWEEP]       = { 0,      Q,        F(1, 2),  true },
        [H5_PARAM_BELL_CYMBAL] = { 0,      0,        0,        false },
    },
    [H5_VOICE_TOM] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(11000), I(11000), true },
        [H5_PARAM_TONE_PITCH]  = { I(28),  I(880),   I(200),   true },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(20),    true },
        [H5_PARAM_DECAY]       = { F(2, 10), F(5, 1), F(1, 1), true },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(7, 10), true },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(7, 10), true },
        [H5_PARAM_SWEEP]       = { 0,      0,        0,        false },
        [H5_PARAM_BELL_CYMBAL] = { 0,      0,        0,        false },
    },
    [H5_VOICE_CYMBAL] = {
        [H5_PARAM_NOISE_PITCH] = { 0,      0,        0,        false },
        [H5_PARAM_TONE_PITCH]  = { 0,      Q,        F(6, 10), true },
        [H5_PARAM_BEND]        = { I(-48), I(48),    I(10),    true },
        [H5_PARAM_DECAY]       = { F(1, 10), F(3, 1), F(3, 1), true },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(1, 2),  true },
        [H5_PARAM_CLICK_TONE]  = { 0,      0,        0,        false },
        [H5_PARAM_SWEEP]       = { I(-1),  I(1),     F(7, 10), true },
        [H5_PARAM_BELL_CYMBAL] = { 0,      Q,        F(1, 2),  true },
    },
    [H5_VOICE_HIHAT] = {
        [H5_PARAM_NOISE_PITCH] = { 0,      0,        0,        false },
        [H5_PARAM_TONE_PITCH]  = { 0,      Q,        Q,        true },
        [H5_PARAM_BEND]        = { I(-48), I(48),    0,        true },
        [H5_PARAM_DECAY]       = { F(1, 10), F(3, 1), F(1, 1), true },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(2, 10), true },
        [H5_PARAM_CLICK_TONE]  = { 0,      0,        0,        false },
        [H5_PARAM_SWEEP]       = { I(-1),  I(1),     0,        true },
        [H5_PARAM_BELL_CYMBAL] = { 0,      Q,        F(1, 2),  true },
    },
};

h5_status h5_set_param(h5_context handle, h5_voice voice, h5_param param,
    int32_t value)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    const struct h5_param_info *info;
    int32_t clamped;
    h5_status status;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return H5_ERR_INVALID_VOICE;
    }
    if ((unsigned)param >= (unsigned)H5_PARAM_COUNT) {
        return H5_ERR_INVALID_PARAM;
    }

    info = &h5_param_table[voice][param];
    if (!info->supported) {
        return H5_ERR_UNSUPPORTED;
    }

    clamped = value;
    status = H5_OK;
    if (clamped < info->min) {
        clamped = info->min;
        status = H5_ERR_RANGE;
    } else if (clamped > info->max) {
        clamped = info->max;
        status = H5_ERR_RANGE;
    }

    ctx->voice_params[voice][param] = clamped;
    h5_voice_configure(ctx, voice);
    return status;
}

h5_status h5_get_param(const h5_context handle, h5_voice voice, h5_param param,
    int32_t *out)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;
    const struct h5_param_info *info;

    if (ctx == 0 || out == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return H5_ERR_INVALID_VOICE;
    }
    if ((unsigned)param >= (unsigned)H5_PARAM_COUNT) {
        return H5_ERR_INVALID_PARAM;
    }

    info = &h5_param_table[voice][param];
    if (!info->supported) {
        return H5_ERR_UNSUPPORTED;
    }

    *out = ctx->voice_params[voice][param];
    return H5_OK;
}

bool h5_param_supported(h5_voice voice, h5_param param)
{
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return false;
    }
    if ((unsigned)param >= (unsigned)H5_PARAM_COUNT) {
        return false;
    }
    return h5_param_table[voice][param].supported;
}
