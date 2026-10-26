#!/usr/bin/env python3
"""Generate the soft-clip shaper table for src/h5_tables.c.

The library shaper is h5_softclip_q31(m) = tanh(1.5 * mix) with mix in Q31
(1.0 = 1 << 31) and the result in Q30 (1.0 = 1 << 30). The table holds the
saturation curve over the argument range [0, 1.5] in H5_SOFTCLIP_STEPS = 24
uniform steps, so entry j holds tanh(1.5 * j / 24) as Q15, rounded half-up:
floor(32767 * tanh(1.5 * j / 24) + 0.5). The table has 25 entries (j = 0..24)
and ends at tanh(1.5) ~= 0.905, which is where the curve is held as |mix|
approaches 1.0; the saturated end is flat enough that the hold is inaudible.
The lookup index is 24 * |mix|, so the entry's argument is 1.5 * |mix|. The
Q15 interpolation is scaled to Q30 by the caller, and negative arguments use
odd symmetry.
"""

import math

SIZE = 25
STEPS = 24
SPAN = 1.5


def main():
    entries = [int(math.floor(32767.0 * math.tanh(SPAN * j / STEPS) + 0.5))
               for j in range(SIZE)]

    print("const int16_t h5_softclip[H5_SOFTCLIP_SIZE] = {")
    for i in range(0, SIZE, 8):
        row = ", ".join("%6d" % v for v in entries[i:i + 8])
        if i + 8 < SIZE:
            print("    " + row + ",")
        else:
            print("    " + row)
    print("};")


main()
