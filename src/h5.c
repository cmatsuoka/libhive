/*
 * h5.c - Hive-V context lifecycle, trigger/mixer API and render entry point.
 *
 * This file provides the context lifecycle, the parameter/mixer/trigger API,
 * and the render entry point. Voices are added incrementally; instruments
 * without an engine yet render silence.
 */

#include "h5.h"
#include "h5_internal.h"

#include <stdlib.h>
#include <string.h>

#define H5_CLEAR_CHUNK 4096u

/* Restore parameters and mixer values to their defaults. The sample rate is
 * left untouched; the caller is responsible for resetting any rate-dependent
 * runtime state. */
static void h5_init_defaults(struct h5_context_s *ctx)
{
    h5_voice v;
    h5_param p;
    h5_instrument i;

    for (v = 0; v < H5_VOICE_COUNT; v++) {
        for (p = 0; p < H5_PARAM_COUNT; p++) {
            ctx->voice_params[v][p] = h5_param_table[v][p].default_value;
        }
    }

    for (i = 0; i < H5_INSTR_COUNT; i++) {
        ctx->instr[i].volume = H5_Q16_ONE / 2;
        ctx->instr[i].pan = H5_Q16_ONE / 2;
    }

    ctx->hihat_open = 0;
}

h5_context h5_create_context(uint32_t sample_rate)
{
    struct h5_context_s *ctx =
        (struct h5_context_s *)calloc(1, sizeof(struct h5_context_s));

    if (ctx == 0) {
        return 0;
    }

    h5_sine_init();
    ctx->sample_rate = (sample_rate == 0u) ? H5_DEFAULT_SAMPLE_RATE
        : sample_rate;
    h5_init_defaults(ctx);
    h5_voice_init_all(ctx);
    return (h5_context)ctx;
}

void h5_destroy_context(h5_context handle)
{
    free(handle);
}

void h5_set_sample_rate(h5_context handle, uint32_t sample_rate)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return;
    }
    ctx->sample_rate = (sample_rate == 0u) ? H5_DEFAULT_SAMPLE_RATE
        : sample_rate;
    ctx->hihat_open = 0;
    h5_voice_init_all(ctx);
}

void h5_reset(h5_context handle)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return;
    }
    h5_init_defaults(ctx);
    h5_voice_reset_all(ctx);
}

static void h5_render_voice(struct h5_context_s *ctx, h5_voice voice,
    h5_instrument instr, int32_t *left, int32_t *right)
{
    int32_t voice_out = h5_voice_process(ctx, voice);
    int32_t mono = h5_mul_q16(voice_out, ctx->instr[instr].volume);

    *left = h5_mul_q16(mono, H5_Q16_ONE - ctx->instr[instr].pan);
    *right = h5_mul_q16(mono, ctx->instr[instr].pan);
}

void h5_process(h5_context handle, int16_t *stereo_out, size_t frames)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    size_t i;

    if (stereo_out == 0) {
        return;
    }

    if (ctx == 0) {
        while (frames > 0u) {
            size_t chunk = (frames < H5_CLEAR_CHUNK) ? frames : H5_CLEAR_CHUNK;
            size_t values = chunk * 2u;

            memset(stereo_out, 0, values * sizeof(int16_t));
            stereo_out += values;
            frames -= chunk;
        }
        return;
    }

    for (i = 0u; i < frames; i++) {
        int32_t kick_l, kick_r, snare_l, snare_r;
        int32_t tom1_l, tom1_r, tom2_l, tom2_r, tom3_l, tom3_r;
        int32_t left, right;

        h5_render_voice(ctx, H5_VOICE_KICK, H5_INSTR_KICK, &kick_l, &kick_r);
        h5_render_voice(ctx, H5_VOICE_SNARE, H5_INSTR_SNARE,
            &snare_l, &snare_r);
        h5_render_voice(ctx, H5_VOICE_TOM_1, H5_INSTR_TOM_1,
            &tom1_l, &tom1_r);
        h5_render_voice(ctx, H5_VOICE_TOM_2, H5_INSTR_TOM_2,
            &tom2_l, &tom2_r);
        h5_render_voice(ctx, H5_VOICE_TOM_3, H5_INSTR_TOM_3,
            &tom3_l, &tom3_r);

        left = h5_sat_add_q31(kick_l, h5_sat_add_q31(snare_l,
            h5_sat_add_q31(tom1_l, h5_sat_add_q31(tom2_l, tom3_l))));
        right = h5_sat_add_q31(kick_r, h5_sat_add_q31(snare_r,
            h5_sat_add_q31(tom1_r, h5_sat_add_q31(tom2_r, tom3_r))));

        stereo_out[2u * i] = h5_q31_to_q15(left);
        stereo_out[2u * i + 1u] = h5_q31_to_q15(right);
    }
}

h5_status h5_set_instrument_volume(h5_context handle, h5_instrument instrument,
    int32_t volume_q16)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    int32_t clamped;
    h5_status status;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }

    clamped = volume_q16;
    status = H5_OK;
    if (clamped < 0) {
        clamped = 0;
        status = H5_ERR_RANGE;
    } else if (clamped > H5_Q16_ONE) {
        clamped = H5_Q16_ONE;
        status = H5_ERR_RANGE;
    }

    ctx->instr[instrument].volume = clamped;
    return status;
}

h5_status h5_set_instrument_pan(h5_context handle, h5_instrument instrument,
    int32_t pan_q16)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    int32_t clamped;
    h5_status status;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }

    clamped = pan_q16;
    status = H5_OK;
    if (clamped < 0) {
        clamped = 0;
        status = H5_ERR_RANGE;
    } else if (clamped > H5_Q16_ONE) {
        clamped = H5_Q16_ONE;
        status = H5_ERR_RANGE;
    }

    ctx->instr[instrument].pan = clamped;
    return status;
}

h5_status h5_get_instrument_volume(const h5_context handle,
    h5_instrument instrument,
    int32_t *out)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;

    if (ctx == 0 || out == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }

    *out = ctx->instr[instrument].volume;
    return H5_OK;
}

h5_status h5_get_instrument_pan(const h5_context handle,
    h5_instrument instrument,
    int32_t *out)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;

    if (ctx == 0 || out == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }

    *out = ctx->instr[instrument].pan;
    return H5_OK;
}

h5_status h5_trigger(h5_context handle, h5_instrument instrument)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }

    if (instrument == H5_INSTR_HIHAT_OPEN) {
        ctx->hihat_open = 1;
    } else if (instrument == H5_INSTR_HIHAT_CLOSED) {
        ctx->hihat_open = 0;
    } else if (instrument == H5_INSTR_KICK) {
        h5_voice_trigger(ctx, H5_VOICE_KICK);
    } else if (instrument == H5_INSTR_TOM_1) {
        h5_voice_trigger(ctx, H5_VOICE_TOM_1);
    } else if (instrument == H5_INSTR_TOM_2) {
        h5_voice_trigger(ctx, H5_VOICE_TOM_2);
    } else if (instrument == H5_INSTR_TOM_3) {
        h5_voice_trigger(ctx, H5_VOICE_TOM_3);
    } else if (instrument == H5_INSTR_SNARE) {
        h5_voice_trigger(ctx, H5_VOICE_SNARE);
    }

    return H5_OK;
}
