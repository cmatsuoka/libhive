#!/usr/bin/env python3
"""Generate the soft-clip shaper table for src/h5_tables.c.

The library shaper is h5_softclip_q31(m) = 1.5 * tanh(u) with u = |m| / 2^31
and the result in Q30 (1.0 = 1 << 30, headroom to 2.0). The lookup index is u with H5_SOFTCLIP_STEPS = 48
uniform steps of 1/48 over the reachable domain [0, 1], so entry j holds
tanh(j/48) as Q15, rounded half-up: floor(32767 * tanh(j/48) + 0.5). The
table has 49 entries (j = 0..48) and ends at tanh(1) ~= 0.76159. The gain 1.5
is applied by the caller multiplying the entry by 49152 (1.5 << 15), making
the Q31-interpolated tanh a Q30 result. Negative arguments use odd symmetry.
"""

import math

SIZE = 49
STEPS_PER_UNIT = 48


def main():
    entries = [int(math.floor(32767.0 * math.tanh(j / STEPS_PER_UNIT) + 0.5))
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