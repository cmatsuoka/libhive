/*
 * wav_writer.c - minimal 16-bit stereo WAV writer for testing
 *
 * Writes a canonical 44 byte RIFF/WAVE header followed by the raw samples.
 * Everything is written little endian by hand, so the file is identical
 * whatever the host byte order is.
 */

#include "wav_writer.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>


static void put_u32(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)(value & 0xFFu);
    dst[1] = (unsigned char)((value >> 8) & 0xFFu);
    dst[2] = (unsigned char)((value >> 16) & 0xFFu);
    dst[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void put_u16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value & 0xFFu);
    dst[1] = (unsigned char)((value >> 8) & 0xFFu);
}

int wav_write_stereo16(const char *path,
    const int16_t *interleaved,
    size_t frames,
    uint32_t sample_rate)
{
    unsigned char header[44];
    const uint16_t channels = 2u;
    const uint16_t bits = 16u;
    const uint32_t block_align = (uint32_t)channels * (uint32_t)(bits / 8u);
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t data_bytes = (uint32_t)(frames * (size_t)channels) * 2u;
    FILE *file;
    size_t written;

    if (path == 0 || (interleaved == 0 && frames > 0u)) {
        return -1;
    }

    file = fopen(path, "wb");
    if (file == 0) {
        return -1;
    }

    memcpy(header + 0, "RIFF", 4);
    put_u32(header + 4, 36u + data_bytes);
    memcpy(header + 8, "WAVE", 4);

    memcpy(header + 12, "fmt ", 4);
    put_u32(header + 16, 16u);        /* PCM chunk size */
    put_u16(header + 20, 1u);         /* format: PCM */
    put_u16(header + 22, channels);
    put_u32(header + 24, sample_rate);
    put_u32(header + 28, byte_rate);
    put_u16(header + 32, (uint16_t)block_align);
    put_u16(header + 34, bits);

    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);

    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return -1;
    }

    written = 0;
    if (data_bytes > 0u) {
        written = fwrite(interleaved, 1, data_bytes, file);
    }

    if (fclose(file) != 0) {
        return -1;
    }
    return (written == data_bytes) ? 0 : -1;
}
