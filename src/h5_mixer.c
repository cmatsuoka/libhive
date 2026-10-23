/*
 * h5_mixer.c - per-instrument volume/pan and stereo output conversion.
 *
 * Each voice produces a mono Q31 sample. The mixer applies the instrument's
 * volume and pan, adds the contribution to the two 64-bit channel
 * accumulators, and converts the completed mix to Q15 once per frame.
 */

#include "h5_internal.h"

void h5_mix_add(const struct h5_context_s *ctx, h5_instrument instrument,
    int32_t voice_out, int64_t *left, int64_t *right)
{
    const int32_t pan = ctx->instr[instrument].pan;
    int32_t mono = h5_mul_q16(voice_out, ctx->instr[instrument].volume);

    *left += h5_mul_q16(mono, H5_Q16_ONE - pan);
    *right += h5_mul_q16(mono, pan);
}

int16_t h5_mix_to_q15(int64_t sum)
{
    int64_t rounded = h5_shr_round(sum, 16u);

    if (rounded > 32767) {
        return 32767;
    }
    if (rounded < -32768) {
        return -32768;
    }
    return (int16_t)rounded;
}
