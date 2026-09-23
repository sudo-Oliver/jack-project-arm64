#!/usr/bin/env python3
"""Turn a faulting GOAL code address into an object + function name.

JIT'd GOAL code has no symbols and no unwind info, so a crash only gives you a raw
kcodeheap address. This maps that address back to a function using two inputs:

  1. A boot log, for the "[EE-LINK] link finish:" lines that record where each object's
     segments were linked at runtime (these move between runs).
  2. A function map from the compiler, which records each function's offset within its
     object's segment (stable for a given build).

Generate the function map with goalc after a full build:

    ./build/goalc/goalc -g jak1 \
        -c '(begin (build-kernel) (build-game) (dump-function-map "/tmp/funcmap.txt"))'

Then resolve the PC and LR from an [EE-CRASH] dump:

    scripts/arm64/resolve_goal_addr.py /tmp/funcmap.txt /tmp/gk.log 0x43e57a0 0x43e5770

Addresses may be given as EE addresses (0x4013270) or as the "code-heap offset" the crash
dump prints (0x13270); the kcodeheap base is added automatically for the latter.
"""

import re
import sys

# kcodeheap occupies this EE address range; segment bases outside it are bogus.
CODE_HEAP_START = 0x4000000
CODE_HEAP_END = 0x5000000

LINK_RE = re.compile(
    r"link finish: (\S+) entry=0x([0-9a-f]+) "
    r"seg0=0x([0-9a-f]+)\+0x([0-9a-f]+) "
    r"seg1=0x([0-9a-f]+)\+0x([0-9a-f]+) "
    r"seg2=0x([0-9a-f]+)\+0x([0-9a-f]+)"
)


def load_segments(log_path):
    """Return [(object_name, seg_index, base, size)] for every real kcodeheap segment.

    v2 level objects print garbage segment data, so anything outside kcodeheap is dropped -
    otherwise those bogus ranges swallow almost any address.
    """
    segments = []
    with open(log_path, errors="ignore") as f:
        for line in f:
            if "[EE-LINK]" not in line:
                continue
            m = LINK_RE.search(line)
            if not m:
                continue
            name = m.group(1)
            for seg in range(3):
                base = int(m.group(3 + 2 * seg), 16)
                size = int(m.group(4 + 2 * seg), 16)
                if size and CODE_HEAP_START <= base < CODE_HEAP_END:
                    segments.append((name, seg, base, size))
    return segments


def load_function_map(map_path):
    """Return {(object_name, seg): [(offset, length, name)]} from dump-function-map output."""
    funcs = {}
    with open(map_path) as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 4)
            if len(parts) != 5:
                continue
            obj, seg, offset, length, name = parts
            funcs.setdefault((obj, int(seg)), []).append((int(offset), int(length), name))
    return funcs


def resolve(addr, segments, funcs):
    if addr < CODE_HEAP_START:
        addr += CODE_HEAP_START  # was a code-heap-relative offset
    for name, seg, base, size in segments:
        if not base <= addr < base + size:
            continue
        off = addr - base
        for f_off, f_len, f_name in funcs.get((name, seg), []):
            if f_off <= off < f_off + f_len:
                return f"{name} seg{seg} +0x{off:x} -> {f_name} +0x{off - f_off:x}"
        return f"{name} seg{seg} +0x{off:x} -> <no function covers this offset>"
    return None


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    funcs = load_function_map(argv[1])
    segments = load_segments(argv[2])
    if not segments:
        print(f"No [EE-LINK] segment lines in {argv[2]} - was the log captured with -v -debug?")
        return 1
    for raw in argv[3:]:
        addr = int(raw, 0)
        result = resolve(addr, segments, funcs)
        print(f"0x{addr:x}: {result or '<not in any linked kcodeheap segment>'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
