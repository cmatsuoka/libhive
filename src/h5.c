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

    for (v = 0; v < H5_VOICE_COUNT; v++) {
        for (p = 0; p < H5_PARAM_COUNT; p++) {
            ctx->voice_params[v][p] = h5_param_table[v][p].default_value;
        }
    }
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
        int64_t left = 0;
        int64_t right = 0;
        int32_t out;

        out = h5_voice_process(ctx, H5_VOICE_KICK);
        if (out != 0) {
            h5_mix_add(ctx, H5_INSTR_KICK, out, &left, &right);
        }
        out = h5_voice_process(ctx, H5_VOICE_SNARE);
        if (out != 0) {
            h5_mix_add(ctx, H5_INSTR_SNARE, out, &left, &right);
        }
        out = h5_voice_process(ctx, H5_VOICE_TOM_1);
        if (out != 0) {
            h5_mix_add(ctx, H5_INSTR_TOM_1, out, &left, &right);
        }
        out = h5_voice_process(ctx, H5_VOICE_TOM_2);
        if (out != 0) {
            h5_mix_add(ctx, H5_INSTR_TOM_2, out, &left, &right);
        }
        out = h5_voice_process(ctx, H5_VOICE_TOM_3);
        if (out != 0) {
            h5_mix_add(ctx, H5_INSTR_TOM_3, out, &left, &right);
        }

        stereo_out[2u * i] = h5_mix_to_q15(left);
        stereo_out[2u * i + 1u] = h5_mix_to_q15(right);
    }
}

h5_status h5_set_instrument_volume(h5_context handle, h5_instrument instrument,
    int32_t volume_q16)
{
    return h5_set_param(handle, (h5_voice)instrument, H5_PARAM_VOLUME,
        volume_q16);
}

h5_status h5_set_instrument_pan(h5_context handle, h5_instrument instrument,
    int32_t pan_q16)
{
    return h5_set_param(handle, (h5_voice)instrument, H5_PARAM_PAN,
        pan_q16);
}

h5_status h5_get_instrument_volume(const h5_context handle,
    h5_instrument instrument,
    int32_t *out)
{
    return h5_get_param(handle, (h5_voice)instrument, H5_PARAM_VOLUME, out);
}

h5_status h5_get_instrument_pan(const h5_context handle,
    h5_instrument instrument,
    int32_t *out)
{
    return h5_get_param(handle, (h5_voice)instrument, H5_PARAM_PAN, out);
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

    h5_voice_trigger_instrument(ctx, instrument);
    return H5_OK;
}
