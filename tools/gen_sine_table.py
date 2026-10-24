#!/usr/bin/env python3
"""Generate the first sine quadrant for src/h5_tables.c.

The stored convention is round-half-up: every entry equals
floor(32767 * sin(2*pi*i/1024) + 0.5) for i = 0..256. The remaining
quadrants are mirrored and negated at run time (see h5_sine_at() and the
H5_SINE_QUAD_SIZE table).
"""

import math


def main():
    entries = [int(math.floor(32767.0 * math.sin(2.0 * math.pi * i / 1024.0)
                               + 0.5))
               for i in range(257)]

    print("const int16_t h5_sine_quad1[H5_SINE_QUAD_SIZE] = {")
    for i in range(0, 257, 8):
        row = ", ".join("%6d" % v for v in entries[i:i + 8])
        if i + 8 < 257:
            print("    " + row + ",")
        else:
            print("    " + row)
    print("};")


main()