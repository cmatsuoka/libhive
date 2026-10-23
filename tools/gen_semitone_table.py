#!/usr/bin/env python3
"""Generate the semitone ratio table for src/h5_tables.c.

The table holds 2^(n/12) as Q16.16 for n = -62..+62 (125 entries), rounded
half-up: floor(2^(n/12) * 65536 + 0.5). Index n + 62. It is used by the Bend
parameter to sweep oscillator frequency without pow().
"""

import math

COUNT = 125
MAX_SEMITONES = 62


def main():
    entries = [int(math.floor((2.0 ** (n / 12.0)) * 65536.0 + 0.5))
               for n in range(-MAX_SEMITONES, MAX_SEMITONES + 1)]

    print("const int32_t h5_semitone[H5_SEMITONE_COUNT] = {")
    for i in range(0, COUNT, 6):
        row = ", ".join("%8d" % v for v in entries[i:i + 6])
        if i + 6 < COUNT:
            print("    " + row + ",")
        else:
            print("    " + row)
    print("};")


main()