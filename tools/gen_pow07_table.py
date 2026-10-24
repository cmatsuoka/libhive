#!/usr/bin/env python3
"""Generate the x^0.7 control table used by the drum voices.

The table covers x = j/64 for j = 0..64, stored as Q14.14 in int16_t with
round-half-up. It is evaluated only when NoiseTone or Click changes, so the
Q14 storage resolution (about 0.003% of full scale) is far below audibility;
linear interpolation supplies the rest. The reader shifts the result back to
Q16.16.
"""

SIZE = 65
ONE = 16384


def main() -> None:
    values = [int((j / (SIZE - 1)) ** 0.7 * ONE + 0.5) for j in range(SIZE)]
    print("const int16_t h5_pow07[H5_POW07_SIZE] = {")
    for i in range(0, SIZE, 6):
        chunk = values[i:i + 6]
        print("    " + ", ".join(f"{v:6d}" for v in chunk) + ",")
    print("};")


if __name__ == "__main__":
    main()
