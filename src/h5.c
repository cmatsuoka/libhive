/*
 * h5.c - Hive-V context lifecycle, trigger/mixer API and render entry point.
 *
 * The voice models are not implemented yet. This file currently provides the
 * context lifecycle, parameter/mixer/trigger API, and a silent renderer so
 * the public API and the test harness can be exercised end to end.
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
    /* Runtime voice state will be re-initialized here once it exists. */
    ctx->hihat_open = 0;
}

void h5_reset(h5_context handle)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return;
    }
    h5_init_defaults(ctx);
}

void h5_process(h5_context handle, int16_t *stereo_out, size_t frames)
{
    (void)handle;

    if (stereo_out == 0) {
        return;
    }

    while (frames > 0u) {
        size_t chunk = (frames < H5_CLEAR_CHUNK) ? frames : H5_CLEAR_CHUNK;
        size_t values = chunk * 2u;

        memset(stereo_out, 0, values * sizeof(int16_t));
        stereo_out += values;
        frames -= chunk;
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
    }

    return H5_OK;
}
