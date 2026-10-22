#!/usr/bin/env python3
"""Generate the first-quadrant sine table used by the Hive-V library.

The library keeps only the first quadrant of the sine in flash as a single
shared `const` table:

    q[i] = floor(32767 * sin(2*pi*i/4096) + 0.5)      for i = 0..1024

Index 0 is the zero crossing, index 1024 the positive peak. The other three
quadrants are rebuilt at run time from this quadrant by mirroring and
negation, so the full period is exact without storing it.

The rounding convention is **round-half-up**, i.e. `floor(x + 0.5)`. The C
harness (`h5_test --sine`) uses the same convention, so the generator and the
test can never disagree about an entry that lands exactly on a half.

Usage:
    python3 tools/gen_sine_table.py            # print the C table on stdout
    python3 tools/gen_sine_table.py --check    # verify only, report on stderr
"""

import math
import sys

FULL_SIZE = 4096
QUAD_SIZE = FULL_SIZE // 4 + 1
Q15_FULL_SCALE = 32767


def q15_sine(index: int) -> int:
    """floor(32767 * sin(2*pi*index/4096) + 0.5), i.e. round-half-up.

    Round-half-up, not Python's banker's rounding, so the value matches the
    C harness (`floor(exact + 0.5)`) exactly for every index.
    """
    exact = Q15_FULL_SCALE * math.sin(2.0 * math.pi * index / FULL_SIZE)
    return math.floor(exact + 0.5)


def quad_table() -> list[int]:
    return [q15_sine(i) for i in range(QUAD_SIZE)]


def rebuild(quad: list[int]) -> list[int]:
    """Expand the first quadrant to the full 4096-entry period."""
    table = [0] * FULL_SIZE
    for i in range(FULL_SIZE):
        if i <= FULL_SIZE // 4:
            table[i] = quad[i]
        elif i <= FULL_SIZE // 2 - 1:
            table[i] = quad[FULL_SIZE // 2 - i]
        elif i <= 3 * FULL_SIZE // 4 - 1:
            table[i] = -quad[i - FULL_SIZE // 2]
        else:
            table[i] = -quad[FULL_SIZE - i]
    return table


def check(quad: list[int]) -> int:
    """Return the number of mismatching entries; print a report to stderr."""
    table = rebuild(quad)
    bad = 0
    worst = 0
    for i in range(FULL_SIZE):
        expected = q15_sine(i)
        err = abs(table[i] - expected)
        worst = max(worst, err)
        if err:
            bad += 1
            if bad <= 8:
                print(
                    f"  mismatch i={i}: got {table[i]}, expected {expected} "
                    f"(err {err})",
                    file=sys.stderr,
                )
    print(f"table: {len(quad)} entries, {len(quad) * 2} bytes", file=sys.stderr)
    print(f"peak  = {quad[FULL_SIZE // 4]} (must be 32767)", file=sys.stderr)
    print(
        f"mismatches = {bad}, worst error = {worst}",
        file=sys.stderr,
    )
    return bad


def emit(quad: list[int]) -> None:
    print("const int16_t h5_sine_quad1[H5_SINE_QUAD_SIZE] = {")
    for start in range(0, QUAD_SIZE, 8):
        row = ", ".join(f"{v:6d}" for v in quad[start : start + 8])
        print(f"    {row},")
    print("};")


def main() -> int:
    quad = quad_table()
    bad = check(quad)
    if bad:
        print("FAIL: the rebuilt table does not match sin(2*pi*i/4096)", file=sys.stderr)
        return 1
    if quad[FULL_SIZE // 4] != Q15_FULL_SCALE:
        print("FAIL: the peak is not exactly 32767", file=sys.stderr)
        return 1
    if "--check" not in sys.argv:
        emit(quad)
    return 0


if __name__ == "__main__":
    sys.exit(main())
