"""Generate the reverse address map as C, from the port's interval map.

The port works out, once per build, a piecewise translation between the reference
executable and the target: a sorted list of [lo, hi) ranges over the reference's
addresses, each with the constant delta that turns a reference address into a target
one.  The runtime has only ever had the *forward* direction, for a handful of
addresses it knows by name.

That is not enough for the tables still written in reference addresses -- for example
the frontend emitter ranges in rn_2d.c -- which are tested against an address taken
from the running build.  On a build that moved, every such test is false, and a false
range test says nothing: the emitters simply stop being recognised, with no warning.

So the whole map is emitted here.  Storing lo and delta per interval is enough,
because hi is the next interval's lo, which halves what has to be kept: six words per
interval would be 633 KB of source, three is 317 KB.  The intervals are already
sorted, disjoint and 4-byte aligned, which this checks rather than assumes, since a
broken map would otherwise be compiled in as a plausible-looking table.
"""

import argparse
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--map", required=True, help="port's addr_map.json")
    ap.add_argument("--out", required=True, help="C file to write")
    ap.add_argument("--check", action="store_true",
                    help="report and exit without writing")
    args = ap.parse_args()

    m = json.load(open(args.map))
    iv = m["intervals"]
    target_lo = m["target_text_addr"]
    target_hi = m["target_text_addr"] + m["target_text_size"]

    # Reverse the direction: the runtime has a target address and wants the
    # reference one, so the table is keyed by where an interval lands in the target.
    rev = []
    for lo, hi, delta in iv:
        t_lo = lo + delta
        t_hi = hi + delta
        if t_lo < 0 or t_hi > 0xFFFFFFFF:
            print("interval %08X..%08X maps outside the address space" % (lo, hi))
            return 1
        # The source side is [lo, hi) itself; delta is what was added to reach the
        # target.  Storing delta as (target - source) keeps the lookup a subtraction.
        rev.append((t_lo, t_hi, delta, lo, hi))
    rev.sort()

    # The forward map is not one-to-one everywhere: in some regions two source
    # intervals with different deltas land on the same target bytes.  The forward
    # direction can still be usable there, but the reverse cannot -- the same target
    # address would translate to two different reference addresses and nothing in the
    # map says which.  Those zones are dropped rather than guessed at, because a
    # guessed reverse translation is worse than none: the caller tests it against a
    # reference range, so a wrong answer is silently a wrong answer, while no answer
    # leaves the caller on the path it would have taken anyway.
    #
    # Two overlapping intervals that agree on the delta are not a conflict at all, just
    # the same translation stated twice, so only disagreements count.  Scanning
    # neighbouring pairs would miss one nested inside another, so this compares every
    # interval against every later one that starts before it ends -- after removing
    # the contained ones, which is what keeps that from being quadratic in practice.
    rev.sort()
    trimmed = []
    for r in rev:
        if trimmed and r[0] == trimmed[-1][0] and r[1] <= trimmed[-1][1]:
            # Same start, no further reach: the wider one already covers it.
            if r[2] == trimmed[-1][2]:
                continue
        trimmed.append(r)
    rev = trimmed

    ambiguous = []
    for i in range(len(rev)):
        t_lo, t_hi, delta = rev[i][0], rev[i][1], rev[i][2]
        j = i + 1
        while j < len(rev) and rev[j][0] < t_hi:
            if rev[j][2] != delta:
                ambiguous.append([rev[j][0], min(t_hi, rev[j][1])])
            j += 1
    ambiguous.sort()
    merged = []
    for lo, hi in ambiguous:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])

    # Subtract the zones from the intervals.  Testing each interval's start against
    # the zones is not enough -- an interval can straddle a zone, keeping its start
    # and so surviving the filter while still overlapping.  Splitting around the zones
    # is the only way to be sure the result is disjoint.
    def subtract(intervals, zones):
        out = list(intervals)
        for z_lo, z_hi in zones:
            nxt = []
            for t_lo, t_hi, delta, s_lo, s_hi in out:
                if t_hi <= z_lo or t_lo >= z_hi:
                    nxt.append((t_lo, t_hi, delta, s_lo, s_hi))
                    continue
                if t_lo < z_lo:
                    nxt.append((t_lo, z_lo, delta, s_lo, s_lo + (z_lo - t_lo)))
                if z_hi < t_hi:
                    nxt.append((z_hi, t_hi, delta,
                                s_lo + (z_hi - t_lo), s_hi))
            out = nxt
        return out

    raw = len(rev)
    rev = subtract(rev, merged)
    rev.sort()
    lost = sum(hi - lo for lo, hi in merged)
    print("%d raw intervals, %d ambiguous zone(s) covering %d target bytes; "
          "%d intervals kept" % (raw, len(merged), lost, len(rev)))

    # With the ambiguous zones gone the rest must tile cleanly; assert it rather than
    # emitting a table that could not have been built without the same guarantee.
    for i in range(1, len(rev)):
        if rev[i][0] < rev[i - 1][1]:
            print("still overlapping after dropping ambiguous zones: "
                  "[%08X,%08X) then [%08X,%08X)"
                  % (rev[i - 1][0], rev[i - 1][1], rev[i][0], rev[i][1]))
            return 1

    # How much of the target text the map actually covers, printed so a partial map is
    # visible rather than assumed.
    covered = sum(t_hi - t_lo for t_lo, t_hi, *_r in rev)
    print("target .text is %d bytes, map covers %d (%.1f%%)"
          % (target_hi - target_lo, covered,
             100.0 * covered / max(1, target_hi - target_lo)))

    if args.check:
        return 0

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/* Generated by tools/gen_revmap.py -- do not edit.\n"
            " *\n"
            " * The reverse of the port's address map: where each range of the loaded\n"
            " * build's addresses came from in the reference build.  Keyed by the\n"
            " * loaded address, because that is what the runtime has when it needs to\n"
            " * test something against a table written in reference addresses.\n"
            " *\n"
            " * %d intervals, from %s (reference) onto %s.\n"
            " */\n\n"
            "#include \"rn_revmap.h\"\n\n"
            "const rn_revmap_entry rn_revmap[] = {\n"
            % (len(rev), os.path.basename(m.get("source_elf", "?")),
               os.path.basename(m.get("target_elf", "?"))))
        # The interval ends where the next one starts, so only the start and the delta
        # are stored.  delta is target - source here, which is what the lookup
        # subtracts; writing it the other way round makes every translation wrong by
        # twice the offset, which looks plausible in the table and is not.
        for t_lo, t_hi, delta, s_lo, s_hi in rev:
            f.write("    { 0x%08Xu, %+#xu },\n" % (t_lo, delta))
        f.write("};\n\n")
        f.write("const unsigned rn_revmap_count = %du;\n" % len(rev))
    print("wrote %s (%d entries, %.0f KB)"
          % (args.out, len(rev), os.path.getsize(args.out) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
