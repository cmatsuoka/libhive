/*
 * h5_fixed.c - linear AR envelope state machine.
 *
 * Implements a linear attack/release envelope: a linear rise
 * over `attack_len` samples to exactly 1.0 (Q30), then a linear fall over
 * `release_len` samples to zero, then silence. The per-sample deltas are
 * computed once per parameter change (h5_env_set_times), never per sample;
 * the per-sample cost is one add.
 *
 * The delta machine is a fixed-point approximation of the ideal linear
 * envelope:
 * inside a phase the accumulated rounding error stays below len/2 LSB of
 * Q30 (well under 0.001), and the peak value is clamped to exactly 1.0, so
 * the error never leaves the attack/release phases.
 */

#include "h5_internal.h"

#define H5_ENV_ONE (1 << 30)

void h5_env_init(struct h5_env_s *e)
{
    e->value = 0;
    e->delta_a = 0;
    e->delta_r = 0;
    e->attack_len = 1;
    e->release_len = 1;
    e->count = 0;
    e->state = H5_ENV_DONE;
}

void h5_env_set_times(struct h5_env_s *e, int32_t attack_q16,
    int32_t release_q16, uint32_t sample_rate)
{
    int64_t an, rn;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    if (attack_q16 < 0) {
        attack_q16 = 0;
    }
    if (release_q16 < 0) {
        release_q16 = 0;
    }

    /* Samples: t[s] * sr, truncated, then 1. Physical value is q16: t =
     * attack_q16 / 2^16. */
    an = (int64_t)(((uint64_t)(uint32_t)attack_q16 * sample_rate) >> 16);
    rn = (int64_t)(((uint64_t)(uint32_t)release_q16 * sample_rate) >> 16);
    if (an < 1) {
        an = 1;
    }
    if (rn < 1) {
        rn = 1;
    }

    /* Attack and release lengths are int32_t, so cap them before scaling.
     * This also bounds the delta division below: with len <= INT32_MAX the
     * numerator (1 << 30) + len/2 is at most INT32_MAX and the quotient is
     * at most 1 << 30, which keeps the whole calculation in 32 bits. */
    if (an > INT32_MAX) {
        an = INT32_MAX;
    }
    if (rn > INT32_MAX) {
        rn = INT32_MAX;
    }
    e->attack_len = (int32_t)an;
    e->release_len = (int32_t)rn;

    /* Deltas in Q30 per sample, rounded to the nearest LSB. */
    e->delta_a = (int32_t)((((uint32_t)1 << 30) + (uint32_t)(an >> 1))
        / (uint32_t)an);
    e->delta_r = (int32_t)((((uint32_t)1 << 30) + (uint32_t)(rn >> 1))
        / (uint32_t)rn);
}

void h5_env_trigger(struct h5_env_s *e)
{
    e->count = 0;
    e->value = 0;
    e->state = H5_ENV_ATTACK;
}

int32_t h5_env_next(struct h5_env_s *e)
{
    switch (e->state) {
    case H5_ENV_ATTACK:
        e->count++;
        if (e->count >= e->attack_len) {
            e->count = 0;
            e->value = H5_ENV_ONE;
            e->state = H5_ENV_RELEASE;
        } else {
            e->value += e->delta_a;
            if (e->value > H5_ENV_ONE) {
                e->value = H5_ENV_ONE;
            }
        }
        break;
    case H5_ENV_RELEASE:
        e->count++;
        e->value -= e->delta_r;
        if (e->value <= 0 || e->count >= e->release_len) {
            e->value = 0;
            e->state = H5_ENV_DONE;
        }
        break;
    default:
        e->value = 0;
        break;
    }
    return e->value;
}