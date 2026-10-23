#!/usr/bin/env python3
"""Generate the soft-clip shaper table for src/h5_tables.c.

The library shaper is h5_softclip_q31(m) = tanh(1.5 * m): the lookup takes
the tanh argument u = 1.5 * m in Q29 over the shaper's actual domain
[0, 1.5), with 97 uniform steps of 1/64. Entry j therefore holds tanh(j/64)
as Q15, rounded half-up: floor(32767 * tanh(j/64) + 0.5). Negative arguments
are handled by odd symmetry; the top entry is tanh(96/64) = tanh(1.5)
~= 0.90515.
"""

import math

SIZE = 97


def main():
    entries = [int(math.floor(32767.0 * math.tanh(j / 64.0) + 0.5))
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