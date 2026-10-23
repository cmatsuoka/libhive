/*
 * h5_voice.c - per-voice lifecycle and dispatch.
 *
 * Engines are added here as they are implemented. Every function tolerates
 * voices that do not have an engine yet and leaves them silent.
 */

#include "h5_internal.h"

void h5_voice_init_all(struct h5_context_s *ctx)
{
    h5_tom_init(&ctx->tom, ctx->sample_rate);
    h5_tom_configure(&ctx->tom, ctx->voice_params[H5_VOICE_TOM],
        ctx->sample_rate);
}

void h5_voice_reset_all(struct h5_context_s *ctx)
{
    h5_voice_init_all(ctx);
}

void h5_voice_configure(struct h5_context_s *ctx, h5_voice voice)
{
    if (voice == H5_VOICE_TOM) {
        h5_tom_configure(&ctx->tom, ctx->voice_params[H5_VOICE_TOM],
            ctx->sample_rate);
    }
}

void h5_voice_trigger(struct h5_context_s *ctx, h5_voice voice)
{
    if (voice == H5_VOICE_TOM) {
        h5_tom_trigger(&ctx->tom);
    }
}

int32_t h5_voice_process(struct h5_context_s *ctx, h5_voice voice)
{
    if (voice == H5_VOICE_TOM) {
        return h5_tom_process(&ctx->tom, ctx->sample_rate);
    }
    return 0;
}
