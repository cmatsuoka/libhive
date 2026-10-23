/*
 * h5_voice.c - per-voice lifecycle and dispatch.
 *
 * Engines are added here as they are implemented. Every function tolerates
 * voices that do not have an engine yet and leaves them silent.
 */

#include "h5_internal.h"

/* Map a Tom voice to its engine index, or -1 for any other voice. The three
 * Tom voices are contiguous in h5_voice. */
static int h5_tom_index(h5_voice voice)
{
    if (voice >= H5_VOICE_TOM_1 && voice <= H5_VOICE_TOM_3) {
        return (int)(voice - H5_VOICE_TOM_1);
    }
    return -1;
}

void h5_voice_init_all(struct h5_context_s *ctx)
{
    int i;

    h5_kick_init(&ctx->kick, ctx->sample_rate);
    h5_kick_configure(&ctx->kick, ctx->voice_params[H5_VOICE_KICK],
        ctx->sample_rate);
    for (i = 0; i < H5_TOM_COUNT; i++) {
        h5_voice voice = (h5_voice)(H5_VOICE_TOM_1 + i);

        h5_tom_init(&ctx->tom[i], voice, ctx->sample_rate);
        h5_tom_configure(&ctx->tom[i], ctx->voice_params[voice],
            ctx->sample_rate);
    }
    h5_snare_init(&ctx->snare, ctx->sample_rate);
    h5_snare_configure(&ctx->snare, ctx->voice_params[H5_VOICE_SNARE],
        ctx->sample_rate);
}

void h5_voice_reset_all(struct h5_context_s *ctx)
{
    h5_voice_init_all(ctx);
}

void h5_voice_configure(struct h5_context_s *ctx, h5_voice voice)
{
    int tom = h5_tom_index(voice);

    if (voice == H5_VOICE_KICK) {
        h5_kick_configure(&ctx->kick, ctx->voice_params[H5_VOICE_KICK],
            ctx->sample_rate);
    } else if (tom >= 0) {
        h5_tom_configure(&ctx->tom[tom], ctx->voice_params[voice],
            ctx->sample_rate);
    } else if (voice == H5_VOICE_SNARE) {
        h5_snare_configure(&ctx->snare, ctx->voice_params[H5_VOICE_SNARE],
            ctx->sample_rate);
    }
}

void h5_voice_trigger(struct h5_context_s *ctx, h5_voice voice)
{
    int tom = h5_tom_index(voice);

    if (voice == H5_VOICE_KICK) {
        h5_kick_trigger(&ctx->kick);
    } else if (tom >= 0) {
        h5_tom_trigger(&ctx->tom[tom]);
    } else if (voice == H5_VOICE_SNARE) {
        h5_snare_trigger(&ctx->snare);
    }
}

void h5_voice_trigger_instrument(struct h5_context_s *ctx,
    h5_instrument instrument)
{
    switch (instrument) {
    case H5_INSTR_KICK:
        h5_voice_trigger(ctx, H5_VOICE_KICK);
        break;
    case H5_INSTR_SNARE:
        h5_voice_trigger(ctx, H5_VOICE_SNARE);
        break;
    case H5_INSTR_TOM_1:
        h5_voice_trigger(ctx, H5_VOICE_TOM_1);
        break;
    case H5_INSTR_TOM_2:
        h5_voice_trigger(ctx, H5_VOICE_TOM_2);
        break;
    case H5_INSTR_TOM_3:
        h5_voice_trigger(ctx, H5_VOICE_TOM_3);
        break;
    default:
        break;
    }
}

int32_t h5_voice_process(struct h5_context_s *ctx, h5_voice voice)
{
    int tom = h5_tom_index(voice);

    if (voice == H5_VOICE_KICK) {
        return h5_kick_process(&ctx->kick, ctx->sample_rate);
    }
    if (tom >= 0) {
        return h5_tom_process(&ctx->tom[tom], ctx->sample_rate);
    }
    if (voice == H5_VOICE_SNARE) {
        return h5_snare_process(&ctx->snare, ctx->sample_rate);
    }
    return 0;
}
