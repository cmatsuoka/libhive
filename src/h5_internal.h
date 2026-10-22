/*
 * h5_internal.h - private definitions shared by the library and its tests.
 *
 * Not part of the public API. Tests include this to inspect context state
 * that the public header deliberately hides.
 */

#ifndef HIVE_V_H5_INTERNAL_H
#define HIVE_V_H5_INTERNAL_H

#include "h5.h"
#include <stdint.h>

/* Full sine period, one entry per sample of a 4096-step cycle. */
#define H5_SINE_TABLE_SIZE 4096

/* First quadrant plus both endpoints: indices 0..1024 inclusive. Stored in
 * flash; the other quadrants are mirrored and negated at run time. */
#define H5_SINE_QUAD_SIZE 1025

/* Description of one (voice, param) entry. */
struct h5_param_info {
    int32_t min;
    int32_t max;
    int32_t default_value;
    bool supported;
};

/* Single source of truth for parameter ranges, defaults and support. */
extern const struct h5_param_info h5_param_table[H5_VOICE_COUNT][H5_PARAM_COUNT];

/* The concrete context behind the opaque h5_context handle. */
struct h5_context_s {
    uint32_t sample_rate;

    /* Per-voice public parameters, stored as Q16.16 physical values. */
    int32_t voice_params[H5_VOICE_COUNT][H5_PARAM_COUNT];

    /* Per-instrument mixer state, Q16.16. */
    struct {
        int32_t volume;
        int32_t pan;
    } instr[H5_INSTR_COUNT];

    /* Hi-hat open/closed gate. Set by h5_trigger(); used by the Hi-hat
     * engine once voices are implemented. */
    uint8_t hihat_open;
};

/* First quadrant of the sine, Q15, for i = 0..1024. This is the only sine
 * data stored in the image; every other entry is derived from it. */
extern const int16_t h5_sine_quad1[H5_SINE_QUAD_SIZE];

/* The full 4096-entry period, allocated and rebuilt from h5_sine_quad1 on the
 * first h5_create_context(). Shared by every context. Null when the
 * allocation failed; the lookups then fall back to the quadrant. */
extern int16_t *h5_sine;

/* Allocate and rebuild the full sine period. Idempotent and meant to be
 * called once, before any lookup. */
void h5_sine_init(void);

/* Q15 sine of 2*pi*i/4096 for any index, using the rebuilt table when it is
 * available and the quadrant fallback otherwise. The index must be 0..4095. */
static inline int16_t h5_sine_at(uint32_t index)
{
    if (h5_sine != 0) {
        return h5_sine[index];
    }
    if (index <= 1024u) {
        return h5_sine_quad1[index];
    }
    if (index <= 2047u) {
        return h5_sine_quad1[2048u - index];
    }
    if (index <= 3071u) {
        return (int16_t)-h5_sine_quad1[index - 2048u];
    }
    return (int16_t)-h5_sine_quad1[4096u - index];
}

#endif /* HIVE_V_H5_INTERNAL_H */
