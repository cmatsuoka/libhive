/*
 * h5.h - public API of the Hive-V library.
 *
 * Hive-V is a five-voice percussion synthesizer arranged as a drum kit:
 * Kick, Snare, Tom 1, Tom 2 and Tom 3. The library renders a single stereo mix
 * into a caller-supplied buffer. It has no I/O and no floating point. A
 * context is created with h5_create_context().
 *
 * Prefixes: macros and enum constants use H5_, functions and types use h5_,
 * and public structures are named h5_context and struct h5_foo.
 */

#ifndef HIVE_V_H5_H
#define HIVE_V_H5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample rate used when the caller passes 0 to h5_create_context() or
 * h5_set_sample_rate(). */
#define H5_DEFAULT_SAMPLE_RATE 44100u

/* Q16.16 fixed-point unit: 1.0 is represented by this integer value. */
#define H5_Q16_ONE 65536

/* The opaque context. */
typedef void *h5_context;

/* The five voice instances, built from three engine types. The Tom engine is
 * instantiated three times so the Toms have independent state and can sound
 * simultaneously. */
typedef enum {
    H5_VOICE_KICK = 0,
    H5_VOICE_SNARE,
    H5_VOICE_TOM_1,
    H5_VOICE_TOM_2,
    H5_VOICE_TOM_3,
    H5_VOICE_COUNT
} h5_voice;

/* The five trigger targets. Each Tom instrument triggers its own Tom voice. */
typedef enum {
    H5_INSTR_KICK = 0,
    H5_INSTR_SNARE,
    H5_INSTR_TOM_1,
    H5_INSTR_TOM_2,
    H5_INSTR_TOM_3,
    H5_INSTR_COUNT
} h5_instrument;

/* Public sound parameters. The standard drum voices (Kick, Snare, Tom 1,
 * Tom 2, Tom 3) share the six faceplate controls Noise, Tone, Bend, Decay,
 * Noise/Tone and Click/Drum. Sweep is exposed for the Snare. Volume and Pan
 * are the per-voice mixer controls, supported on every voice. Not every
 * parameter applies to every voice; h5_param_supported() reports the supported
 * pairs. */
typedef enum {
    H5_PARAM_NOISE_PITCH = 0,
    H5_PARAM_TONE_PITCH,
    H5_PARAM_BEND,
    H5_PARAM_DECAY,
    H5_PARAM_NOISE_TONE,
    H5_PARAM_CLICK_TONE,
    H5_PARAM_SWEEP,
    H5_PARAM_VOLUME,
    H5_PARAM_PAN,
    H5_PARAM_COUNT
} h5_param;

/* Return status for setters and getters. */
typedef enum {
    H5_OK = 0,
    H5_ERR_INVALID_CONTEXT,
    H5_ERR_INVALID_VOICE,
    H5_ERR_INVALID_INSTRUMENT,
    H5_ERR_INVALID_PARAM,
    H5_ERR_UNSUPPORTED,
    H5_ERR_RANGE
} h5_status;

/* Allocate and initialize a context. A sample_rate of 0 selects
 * H5_DEFAULT_SAMPLE_RATE. Returns NULL when the allocation fails.
 *
 * The caller owns the returned context and releases it with
 * h5_destroy_context(). */
h5_context h5_create_context(uint32_t sample_rate);

/* Release a context returned by h5_create_context(). A null context is
 * ignored. Every context must be released exactly once. */
void h5_destroy_context(h5_context ctx);

/* Change the sample rate. Internal state that depends on the rate is
 * re-initialized, so any sounding voice is cut. */
void h5_set_sample_rate(h5_context ctx, uint32_t sample_rate);

/* Reset the voices, parameters and the mix controls to their defaults, keeping
 * the sample rate. */
void h5_reset(h5_context ctx);

/* Render `frames` stereo interleaved frames into `stereo_out`.
 *
 * `stereo_out` must have room for `frames` stereo frames (2 * frames int16_t
 * values). The library is not thread-safe: h5_process() and the setters must
 * never run at the same time. h5_create_context() and h5_destroy_context()
 * must not run at the same time either. */
void h5_process(h5_context ctx, int16_t *stereo_out, size_t frames);

/* Set a sound parameter for a voice. Values are Q16.16 physical units:
 * Hz for pitch and filter frequencies, seconds for decay, signed semitones
 * for bend, and a 0..1 scalar for balance parameters.
 *
 * Out-of-range values are clamped to the documented range and H5_ERR_RANGE is
 * returned; the clamped value is stored. Unsupported (voice, param) pairs
 * return H5_ERR_UNSUPPORTED and leave state unchanged. */
h5_status h5_set_param(h5_context ctx,
    h5_voice voice,
    h5_param param,
    int32_t value);

/* Read back a voice parameter. Returns the stored Q16.16 value in *out.
 * A null *out is rejected with H5_ERR_INVALID_CONTEXT. */
h5_status h5_get_param(const h5_context ctx,
    h5_voice voice,
    h5_param param,
    int32_t *out);

/* Return true when the (voice, param) pair is part of the public control
 * surface for that voice. */
bool h5_param_supported(h5_voice voice, h5_param param);

/* Set the per-instrument mixer volume. Range is 0 .. H5_Q16_ONE;
 * out-of-range values are clamped to that range, the clamped value is
 * stored, and H5_ERR_RANGE is returned. Equivalent to
 * h5_set_param(ctx, (h5_voice)instrument, H5_PARAM_VOLUME, volume_q16),
 * kept as a function (not a macro) so arguments stay type-checked. */
h5_status h5_set_instrument_volume(h5_context ctx,
    h5_instrument instrument,
    int32_t volume_q16);

/* Set the per-instrument mixer pan. Range is 0 .. H5_Q16_ONE;
 * 0 = left, H5_Q16_ONE / 2 = centre, H5_Q16_ONE = right. Out-of-range
 * values are clamped to that range, the clamped value is stored, and
 * H5_ERR_RANGE is returned. Equivalent to
 * h5_set_param(ctx, (h5_voice)instrument, H5_PARAM_PAN, pan_q16). */
h5_status h5_set_instrument_pan(h5_context ctx,
    h5_instrument instrument,
    int32_t pan_q16);

/* Read back the per-instrument volume in Q16.16. A null *out is rejected
 * with H5_ERR_INVALID_CONTEXT. Equivalent to h5_get_param() on
 * H5_PARAM_VOLUME. */
h5_status h5_get_instrument_volume(const h5_context ctx,
    h5_instrument instrument,
    int32_t *out);

/* Read back the per-instrument pan in Q16.16. A null *out is rejected
 * with H5_ERR_INVALID_CONTEXT. Equivalent to h5_get_param() on
 * H5_PARAM_PAN. */
h5_status h5_get_instrument_pan(const h5_context ctx,
    h5_instrument instrument,
    int32_t *out);

/* Trigger one of the five instruments. Each Tom instrument starts its own Tom
 * voice, so Tom 1, Tom 2 and Tom 3 can sound simultaneously. */
h5_status h5_trigger(h5_context ctx, h5_instrument instrument);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HIVE_V_H5_H */
