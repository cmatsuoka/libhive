/*
 * tests.c - per-voice render checks.
 *
 * Each command triggers one voice through the public API, renders a single
 * hit, writes a WAV file for listening and checks the envelope, the tail and
 * the direction of the main controls.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h5.h"
#include "tests.h"
#include "wav_writer.h"

#define TEST_RATE 44100u

static int failures = 0;

static void fail(const char *what)
{
    printf("FAIL: %s\n", what);
    failures++;
}

static void ok(const char *what)
{
    printf("ok  : %s\n", what);
}

static size_t frames_for_ms(uint32_t sample_rate, uint32_t ms)
{
    return (size_t)((uint64_t)sample_rate * ms / 1000u);
}

static int32_t sample_abs(int16_t v)
{
    return (v < 0) ? -(int32_t)v : (int32_t)v;
}

static int32_t peak_abs(const int16_t *buffer, size_t begin, size_t end)
{
    int32_t peak = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int32_t v = sample_abs(buffer[2u * i]);

        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static size_t first_above(const int16_t *buffer, size_t frames, int32_t level)
{
    size_t i;

    for (i = 0u; i < frames; i++) {
        if (sample_abs(buffer[2u * i]) > level) {
            return i;
        }
    }
    return frames;
}

static size_t last_above(const int16_t *buffer, size_t frames, int32_t level)
{
    size_t i = frames;

    while (i > 0u) {
        i--;
        if (sample_abs(buffer[2u * i]) > level) {
            return i;
        }
    }
    return 0u;
}

static int64_t energy(const int16_t *buffer, size_t begin, size_t end)
{
    int64_t sum = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int32_t v = buffer[2u * i];

        sum += (int64_t)v * v;
    }
    return sum;
}

static unsigned zero_crossings(const int16_t *buffer, size_t begin, size_t end)
{
    unsigned crossings = 0;
    int previous = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int current = buffer[2u * i];

        if (current == 0) {
            continue;
        }
        if (previous != 0 && ((previous < 0) != (current < 0))) {
            crossings++;
        }
        previous = current;
    }
    return crossings;
}

static int16_t *render(h5_context ctx, size_t frames)
{
    int16_t *buffer = (int16_t *)malloc(frames * 2u * sizeof(int16_t));

    if (buffer == 0) {
        return 0;
    }
    h5_process(ctx, buffer, frames);
    return buffer;
}

static void set_tom(h5_context ctx, h5_param param, int32_t value)
{
    h5_set_param(ctx, H5_VOICE_TOM, param, value);
}

static int command_tom_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    /* Mid-tom tuning for the hit test: lower pitch, moderate decay and a
     * smaller noise contribution than the default voice. */
    set_tom(ctx, H5_PARAM_TONE_PITCH, 130 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_NOISE_PITCH, 7000 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 1 * H5_Q16_ONE / 2);
    set_tom(ctx, H5_PARAM_BEND, 10 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_DECAY, 8 * H5_Q16_ONE / 10);

    h5_trigger(ctx, H5_INSTR_TOM);
    buffer = render(ctx, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }

    first = first_above(buffer, frames, 16);
    last = last_above(buffer, frames, 16);
    peak = peak_abs(buffer, 0u, frames);
    tail_peak = peak_abs(buffer, check_start, frames);
    crossings = zero_crossings(buffer, 0u, frames_for_ms(TEST_RATE, 100u));

    if (first > frames_for_ms(TEST_RATE, 10u)) {
        fail("the Tom hit does not start promptly");
    } else {
        ok("the Tom hit starts promptly");
    }

    if (tail_peak > 1) {
        fail("the Tom default tail does not decay to silence");
    } else {
        ok("the Tom default tail decays to silence");
    }

    if (peak < 1000 || last <= first) {
        fail("the Tom default hit is silent or degenerate");
    } else {
        ok("the Tom default hit is audible");
    }

    if (crossings < 5u || crossings > 2000u) {
        fail("the Tom default timbre is outside the expected band");
    } else {
        ok("the Tom default timbre is in the expected band");
    }

    if (wav_write_stereo16("build/tom.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/tom.wav");
    } else {
        printf("ok  : wrote build/tom.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return 0;
}

static int command_tom_pitch(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *low;
    int16_t *high;
    unsigned zc_low, zc_high;

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 100 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_TOM);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 400 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_TOM);
    high = render(ctx, frames);

    if (low == 0 || high == 0) {
        fail("out of memory");
        free(low);
        free(high);
        return -1;
    }

    zc_low = zero_crossings(low, 0u, window);
    zc_high = zero_crossings(high, 0u, window);

    if (zc_high < 2u * zc_low) {
        fail("Tom TonePitch does not raise the tone frequency");
    } else {
        ok("Tom TonePitch raises the tone frequency");
    }

    free(low);
    free(high);
    return 0;
}

static int command_tom_decay(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 2000u);
    int16_t *short_hit;
    int16_t *long_hit;
    size_t short_end, long_end;

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 5);
    h5_trigger(ctx, H5_INSTR_TOM);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_TOM);
    long_hit = render(ctx, frames);

    if (short_hit == 0 || long_hit == 0) {
        fail("out of memory");
        free(short_hit);
        free(long_hit);
        return -1;
    }

    short_end = last_above(short_hit, frames, 8);
    long_end = last_above(long_hit, frames, 8);

    if (long_end <= short_end + frames_for_ms(TEST_RATE, 300u)) {
        fail("Tom Decay does not lengthen the tail");
    } else {
        ok("Tom Decay lengthens the tail");
    }

    free(short_hit);
    free(long_hit);
    return 0;
}

static int command_tom_noise(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *tone;
    int16_t *noise;
    unsigned zc_tone, zc_noise;

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    h5_trigger(ctx, H5_INSTR_TOM);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    h5_trigger(ctx, H5_INSTR_TOM);
    noise = render(ctx, frames);

    if (tone == 0 || noise == 0) {
        fail("out of memory");
        free(tone);
        free(noise);
        return -1;
    }

    zc_tone = zero_crossings(tone, 0u, window);
    zc_noise = zero_crossings(noise, 0u, window);

    if (zc_noise < 2u * zc_tone) {
        fail("Tom NoiseTone does not add high-frequency noise");
    } else {
        ok("Tom NoiseTone adds high-frequency noise");
    }

    /* The noise path has full weight and the tone path a 0.3 headroom
     * factor, so a fully noise-weighted hit must carry at least as much
     * energy as a fully tone-weighted hit. */
    if (energy(noise, 0u, window) <= energy(tone, 0u, window)) {
        fail("Tom noise path is quieter than the tone path");
    } else {
        ok("Tom noise path is not quieter than the tone path");
    }

    free(tone);
    free(noise);
    return 0;
}

int command_tom(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_tom_hit(ctx) != 0 ||
        command_tom_pitch(ctx) != 0 ||
        command_tom_decay(ctx) != 0 ||
        command_tom_noise(ctx) != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}
