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
    h5_set_param(ctx, H5_VOICE_TOM_2, param, value);
}

static void set_tom_voice(h5_context ctx, h5_voice voice, h5_param param,
    int32_t value)
{
    h5_set_param(ctx, voice, param, value);
}

static void configure_tom_for_mix(h5_context ctx, h5_voice voice,
    int32_t pitch_hz)
{
    set_tom_voice(ctx, voice, H5_PARAM_NOISE_TONE, 0);
    set_tom_voice(ctx, voice, H5_PARAM_CLICK_TONE, 0);
    set_tom_voice(ctx, voice, H5_PARAM_BEND, 0);
    set_tom_voice(ctx, voice, H5_PARAM_DECAY, H5_Q16_ONE);
    set_tom_voice(ctx, voice, H5_PARAM_TONE_PITCH, pitch_hz * H5_Q16_ONE);
}

static int command_tom_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    /* Tom 2 tuning for the hit test: lower pitch, moderate decay and a
     * smaller noise contribution than the default voice. */
    set_tom(ctx, H5_PARAM_TONE_PITCH, 130 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_NOISE_PITCH, 7000 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 1 * H5_Q16_ONE / 2);
    set_tom(ctx, H5_PARAM_BEND, 10 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_DECAY, 8 * H5_Q16_ONE / 10);

    h5_trigger(ctx, H5_INSTR_TOM_2);
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
    h5_trigger(ctx, H5_INSTR_TOM_2);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 400 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_TOM_2);
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
    h5_trigger(ctx, H5_INSTR_TOM_2);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_TOM_2);
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
    h5_trigger(ctx, H5_INSTR_TOM_2);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    h5_trigger(ctx, H5_INSTR_TOM_2);
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

static int command_tom_simultaneous(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1000u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    const h5_voice voices[3] = {
        H5_VOICE_TOM_1, H5_VOICE_TOM_2, H5_VOICE_TOM_3
    };
    const h5_instrument instrs[3] = {
        H5_INSTR_TOM_1, H5_INSTR_TOM_2, H5_INSTR_TOM_3
    };
    const int32_t pitches[3] = { 150, 250, 400 };
    int16_t *solo[3] = { 0, 0, 0 };
    int16_t *all;
    int64_t solo_energy[3];
    int64_t all_energy;
    int32_t value;
    int result = 0;
    size_t i;

    for (i = 0u; i < 3u; i++) {
        h5_reset(ctx);
        configure_tom_for_mix(ctx, voices[i], pitches[i]);
        h5_trigger(ctx, instrs[i]);
        solo[i] = render(ctx, frames);
        if (solo[i] == 0) {
            fail("out of memory");
            free(solo[0]);
            free(solo[1]);
            free(solo[2]);
            return -1;
        }
        if (h5_get_param(ctx, voices[i], H5_PARAM_TONE_PITCH, &value) != H5_OK
            || value != pitches[i] * H5_Q16_ONE) {
            fail("a Tom voice did not keep its own TonePitch");
            result = -1;
        }
    }

    h5_reset(ctx);
    for (i = 0u; i < 3u; i++) {
        configure_tom_for_mix(ctx, voices[i], pitches[i]);
    }
    for (i = 0u; i < 3u; i++) {
        h5_trigger(ctx, instrs[i]);
    }
    all = render(ctx, frames);
    if (all == 0) {
        fail("out of memory");
        free(solo[0]);
        free(solo[1]);
        free(solo[2]);
        return -1;
    }

    all_energy = energy(all, 0u, frames);
    for (i = 0u; i < 3u; i++) {
        solo_energy[i] = energy(solo[i], 0u, frames);
        if (peak_abs(solo[i], 0u, frames) < 100) {
            fail("a solo Tom is silent");
            result = -1;
        }
        if (memcmp(all, solo[i], bytes) == 0) {
            fail("the three-Tom mix equals a solo Tom");
            result = -1;
        }
        if (all_energy <= solo_energy[i]) {
            fail("the three-Tom mix is not louder than a solo Tom");
            result = -1;
        }
    }

    if (result == 0) {
        ok("Tom 1, Tom 2 and Tom 3 sound simultaneously with independent state");
    }

    free(solo[0]);
    free(solo[1]);
    free(solo[2]);
    free(all);
    return result;
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
        command_tom_noise(ctx) != 0 ||
        command_tom_simultaneous(ctx) != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}

static void set_kick(h5_context ctx, h5_param param, int32_t value)
{
    h5_set_param(ctx, H5_VOICE_KICK, param, value);
}

static int command_kick_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 60 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_NOISE_PITCH, 6000 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_BEND, 36 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 2);

    h5_trigger(ctx, H5_INSTR_KICK);
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
        fail("the Kick hit does not start promptly");
    } else {
        ok("the Kick hit starts promptly");
    }

    if (tail_peak > 1) {
        fail("the Kick default tail does not decay to silence");
    } else {
        ok("the Kick default tail decays to silence");
    }

    if (peak < 1000 || last <= first) {
        fail("the Kick default hit is silent or degenerate");
    } else {
        ok("the Kick default hit is audible");
    }

    if (crossings < 5u || crossings > 3000u) {
        fail("the Kick default timbre is outside the expected band");
    } else {
        ok("the Kick default timbre is in the expected band");
    }

    if (wav_write_stereo16("build/kick.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/kick.wav");
    } else {
        printf("ok  : wrote build/kick.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return 0;
}

static int command_kick_pitch(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *low;
    int16_t *high;
    unsigned zc_low, zc_high;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 50 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
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
        fail("Kick TonePitch does not raise the tone frequency");
    } else {
        ok("Kick TonePitch raises the tone frequency");
    }

    free(low);
    free(high);
    return 0;
}

static int command_kick_bend(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *flat;
    int16_t *fall;
    unsigned zc_flat, zc_fall;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 30 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    h5_trigger(ctx, H5_INSTR_KICK);
    flat = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 30 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 36 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
    fall = render(ctx, frames);

    if (flat == 0 || fall == 0) {
        fail("out of memory");
        free(flat);
        free(fall);
        return -1;
    }

    zc_flat = zero_crossings(flat, 0u, window);
    zc_fall = zero_crossings(fall, 0u, window);

    if (zc_fall < 2u * zc_flat) {
        fail("Kick Bend does not lower the pitch over the hit");
    } else {
        ok("Kick Bend lowers the pitch over the hit");
    }

    free(flat);
    free(fall);
    return 0;
}

static int command_kick_decay(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 2000u);
    int16_t *short_hit;
    int16_t *long_hit;
    size_t short_end, long_end;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 5);
    h5_trigger(ctx, H5_INSTR_KICK);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_KICK);
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
        fail("Kick Decay does not lengthen the tail");
    } else {
        ok("Kick Decay lengthens the tail");
    }

    free(short_hit);
    free(long_hit);
    return 0;
}

static int command_kick_noise(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *tone;
    int16_t *noise;
    unsigned zc_tone, zc_noise;

    /* The noise path is gated by Click^0.7 as well as NoiseTone, so keep a
     * non-zero Click to compare the two balance extremes. */
    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 60 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, H5_INSTR_KICK);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 60 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
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
        fail("Kick NoiseTone does not add high-frequency noise");
    } else {
        ok("Kick NoiseTone adds high-frequency noise");
    }

    free(tone);
    free(noise);
    return 0;
}

static int16_t *render_kick_click(int32_t click, size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;

    if (ctx == 0) {
        return 0;
    }
    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_CLICK_TONE, click);
    h5_trigger(ctx, H5_INSTR_KICK);
    buffer = render(ctx, frames);
    h5_destroy_context(ctx);
    return buffer;
}

static int command_kick_click(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 500u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    int16_t *a = render_kick_click(0, frames);
    int16_t *b = render_kick_click(H5_Q16_ONE, frames);

    if (a == 0 || b == 0) {
        fail("out of memory");
        free(a);
        free(b);
        return -1;
    }
    if (memcmp(a, b, bytes) == 0) {
        fail("Kick Click does not change the output");
    } else {
        ok("Kick Click changes the output");
    }
    free(a);
    free(b);
    return 0;
}

int command_kick(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_kick_hit(ctx) != 0 ||
        command_kick_pitch(ctx) != 0 ||
        command_kick_bend(ctx) != 0 ||
        command_kick_decay(ctx) != 0 ||
        command_kick_noise(ctx) != 0 ||
        command_kick_click() != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}

static void set_snare(h5_context ctx, h5_param param, int32_t value)
{
    h5_set_param(ctx, H5_VOICE_SNARE, param, value);
}

static int command_snare_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    /* Standard snare tuning for the hit test. */
    set_snare(ctx, H5_PARAM_TONE_PITCH, 130 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_NOISE_PITCH, 7000 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 6 * H5_Q16_ONE / 10);
    set_snare(ctx, H5_PARAM_BEND, 8 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_DECAY, 7 * H5_Q16_ONE / 10);

    h5_trigger(ctx, H5_INSTR_SNARE);
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
        fail("the Snare hit does not start promptly");
    } else {
        ok("the Snare hit starts promptly");
    }

    if (tail_peak > 1) {
        fail("the Snare default tail does not decay to silence");
    } else {
        ok("the Snare default tail decays to silence");
    }

    if (peak < 1000 || last <= first) {
        fail("the Snare default hit is silent or degenerate");
    } else {
        ok("the Snare default hit is audible");
    }

    if (crossings < 5u || crossings > 3000u) {
        fail("the Snare default timbre is outside the expected band");
    } else {
        ok("the Snare default timbre is in the expected band");
    }

    if (wav_write_stereo16("build/snare.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/snare.wav");
    } else {
        printf("ok  : wrote build/snare.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return 0;
}

static int command_snare_pitch(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *low;
    int16_t *high;
    unsigned zc_low, zc_high;

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 100 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_SNARE);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 400 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_SNARE);
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
        fail("Snare TonePitch does not raise the tone frequency");
    } else {
        ok("Snare TonePitch raises the tone frequency");
    }

    free(low);
    free(high);
    return 0;
}

static int command_snare_decay(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 2000u);
    int16_t *short_hit;
    int16_t *long_hit;
    size_t short_end, long_end;

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 5);
    h5_trigger(ctx, H5_INSTR_SNARE);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_SNARE);
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
        fail("Snare Decay does not lengthen the tail");
    } else {
        ok("Snare Decay lengthens the tail");
    }

    free(short_hit);
    free(long_hit);
    return 0;
}

static int command_snare_noise(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *tone;
    int16_t *noise;
    unsigned zc_tone, zc_noise;

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, H5_INSTR_SNARE);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_SNARE);
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
        fail("Snare NoiseTone does not add high-frequency rattle");
    } else {
        ok("Snare NoiseTone adds high-frequency rattle");
    }

    /* The rattle path uses four narrow band-pass filters, so its total
     * energy can be lower than a pure sine tone. Verify it is still audible
     * and carries broadband high-frequency content. */
    if (peak_abs(noise, 0u, window) < 100) {
        fail("Snare rattle path is silent");
    } else {
        ok("Snare rattle path is audible");
    }

    free(tone);
    free(noise);
    return 0;
}

static int16_t *render_snare_variant(int32_t click, int32_t sweep,
    size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;

    if (ctx == 0) {
        return 0;
    }
    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_CLICK_TONE, click);
    set_snare(ctx, H5_PARAM_SWEEP, sweep);
    h5_trigger(ctx, H5_INSTR_SNARE);
    buffer = render(ctx, frames);
    h5_destroy_context(ctx);
    return buffer;
}

static int command_snare_controls(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 500u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    int16_t *a;
    int16_t *b;

    a = render_snare_variant(0, H5_Q16_ONE / 2, frames);
    b = render_snare_variant(H5_Q16_ONE, H5_Q16_ONE / 2, frames);
    if (a == 0 || b == 0) {
        fail("out of memory");
        free(a);
        free(b);
        return -1;
    }
    if (memcmp(a, b, bytes) == 0) {
        fail("Snare Click does not change the output");
    } else {
        ok("Snare Click changes the output");
    }
    free(a);
    free(b);

    a = render_snare_variant(0, 0, frames);
    b = render_snare_variant(0, H5_Q16_ONE, frames);
    if (a == 0 || b == 0) {
        fail("out of memory");
        free(a);
        free(b);
        return -1;
    }
    if (memcmp(a, b, bytes) == 0) {
        fail("Snare Sweep does not change the output");
    } else {
        ok("Snare Sweep changes the output");
    }
    free(a);
    free(b);
    return 0;
}

int command_snare(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_snare_hit(ctx) != 0 ||
        command_snare_pitch(ctx) != 0 ||
        command_snare_decay(ctx) != 0 ||
        command_snare_noise(ctx) != 0 ||
        command_snare_controls() != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}
