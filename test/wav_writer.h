/*
 * wav_writer.h - minimal 16-bit stereo WAV writer for testing
 *
 * Not part of the library. The test harness uses it to write rendered WAV
 * files for listening and comparison.
 */

#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stddef.h>
#include <stdint.h>

/* Write `frames` interleaved stereo 16-bit samples as a canonical
 * 44-byte-header RIFF/WAVE file. Returns 0 on success, -1 on error. */
int wav_write_stereo16(const char *path,
    const int16_t *interleaved,
    size_t frames,
    uint32_t sample_rate);

#endif /* WAV_WRITER_H */
