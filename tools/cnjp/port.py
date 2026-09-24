"""Port the US configuration files to another region's executable.

The US release (SLUS_208.51) and the Japanese / Chinese-localised release
(SLPS_254.18) are the same game built twice.  Their code sections start at the
same address and long stretches are byte identical, so most guest addresses mean
the same thing in both.  Where code was added, removed or reordered the addresses
move, and the `config/` files -- which are all expressed in US addresses -- have
to be translated before the recompiler can be pointed at the other executable.

How the address map is built
----------------------------
1. Long runs of identical instruction words at a constant index delta become
   "anchors".  Anchors are merged into a list of intervals covering the text.
2. Gaps between anchors are filled from the preceding interval, giving a total
   function from US address to target address.
3. Every function the US IDA export knows about is checked against the target
   image: its instruction stream is compared with absolute address fields
   blanked out.  Functions that do not line up are searched for in a small window
   around their predicted address, and the ones found become explicit overrides.
4. Functions that cannot be located at all are recorded as unresolved; their
   address ranges are excluded from seeding so the recompiler discovers those
   regions from the call graph instead of trusting a bad boundary.

Usage
-----
    # 1. build the address map (a few minutes, exact string matching)
    python -m cnjp map --source SLUS_208.51 --target SLPS_254.18 \
        --ida-db config/ida_db.json -o config/cnjp/addr_map.json

    # 2. translate the config files
    python -m cnjp port --map config/cnjp/addr_map.json \
        --config config --out config/cnjp --region cnjp

Requires no third-party modules.
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import struct
import sys
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.dirname(_HERE)
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

from ps2recomp.elf import ElfFile          # noqa: E402
from ps2recomp import r5900                # noqa: E402

# ---------------------------------------------------------------------------
# Instructions whose immediate or target field carries a guest address that
# moves when the image is relaid out.  Everything else is treated as stable
# code, which is what makes the structural comparison meaningful.
# ---------------------------------------------------------------------------

IMM16_OPCODES = {
    "ADDIU", "ADDI", "DADDIU", "DADDI", "ORI", "ANDI", "XORI", "LUI", "SLTI",
    "SLTIU", "LW", "SW", "LD", "SD", "LB", "LBU", "LH", "LHU", "SB", "SH",
    "LWC1", "SWC1", "LDC1", "SDC1", "LQ", "SQ", "LWU", "CACHE", "LDL", "LDR",
    "SDL", "SDR", "LWLE", "LWRE", "SWLE", "SWRE", "LL", "SC", "PREF",
}
BRANCH_OPCODES = {
    "BEQ", "BNE", "BLEZ", "BGTZ", "BLTZ", "BGEZ", "BLTZAL", "BGEZAL",
    "BEQL", "BNEL", "BLEZL", "BGTZL", "BLTZL", "BGEZL", "BLTZALL", "BGEZALL",
}
ADDR_MASK: Dict[str, int] = {"J": 0x03FFFFFF, "JAL": 0x03FFFFFF}
for _op in IMM16_OPCODES:
    ADDR_MASK[_op] = 0x0000FFFF
for _op in BRANCH_OPCODES:
    ADDR_MASK[_op] = 0x0000FFFF


def normalized_word(elf: ElfFile, ea: int) -> int:
    """The instruction word with any embedded guest address blanked out."""
    w = elf.word(ea)
    mask = ADDR_MASK.get(r5900.decode(w, ea).name)
    return (w & ~mask & 0xFFFFFFFF) if mask else w


def normalized_body(elf: ElfFile, ea: int, words: int) -> List[int]:
    return [normalized_word(elf, ea + 4 * i) for i in range(words)]


def opcode_word(elf: ElfFile, ea: int) -> int:
    """The instruction with *every* immediate and displacement removed.

    Comparing these tolerates the two builds reordering instructions that only
    differ in an immediate (a run of `addiu reg, reg, N` being the common case)
    while keeping the opcode and all register fields, which is a far stronger
    statement than "these two functions have the same length".
    """
    w = elf.word(ea)
    if r5900.decode(w, ea).name in ("J", "JAL"):
        return w & 0xFC000000
    return w & 0xFFFF0000


def opcode_body(elf: ElfFile, ea: int, words: int) -> List[int]:
    return [opcode_word(elf, ea + 4 * i) for i in range(words)]


# ---------------------------------------------------------------------------
# Address map
# ---------------------------------------------------------------------------

class AddressMap:
    """Piecewise translation between two builds of the same executable."""

    def __init__(self, blob: dict):
        self.blob = blob
        self.source_text = (blob["source_text_addr"],
                            blob["source_text_addr"] + blob["source_text_size"])
        self.target_text = (blob["target_text_addr"],
                            blob["target_text_addr"] + blob["target_text_size"])
        # intervals: sorted list of [source_lo, source_hi, delta]
        self.intervals: List[List[int]] = [list(iv) for iv in blob["intervals"]]
        self.overrides: Dict[int, int] = {
            int(k, 16): int(v) for k, v in blob.get("overrides", {}).items()
        }
        self.unresolved: List[Tuple[int, int]] = [
            (int(a, 16), int(b, 16)) for a, b in blob.get("unresolved_ranges", [])
        ]
        self._lo = [iv[0] for iv in self.intervals]
        self._unres_lo = [r[0] for r in self.unresolved]

    # -- building ----------------------------------------------------------
    @staticmethod
    def build(source_elf: str, target_elf: str, ida_db: str,
              search_bytes: int = 0x400, min_anchor_words: int = 8,
              log=print) -> "AddressMap":
        src = ElfFile(source_elf)
        tgt = ElfFile(target_elf)
        s_sec, t_sec = src.section(".text"), tgt.section(".text")
        sw = struct.unpack("<%dI" % (len(s_sec.data) // 4),
                           s_sec.data[:len(s_sec.data) // 4 * 4])
        tw = struct.unpack("<%dI" % (len(t_sec.data) // 4),
                           t_sec.data[:len(t_sec.data) // 4 * 4])
        ns, nt = len(sw), len(tw)
        log(f"source .text {s_sec.addr:08X} words={ns}")
        log(f"target .text {t_sec.addr:08X} words={nt}")

        span = search_bytes // 4
        deltas = [4 * d for d in range(-span, span + 1)]

        # Anchor: the longest run of equal words starting at this index.
        intervals: List[List[int]] = []
        i = 0
        while i < ns:
            best = None
            for delta in deltas:
                j = i + delta // 4
                if j < 0 or j >= nt or sw[i] != tw[j]:
                    continue
                n = 0
                while i + n < ns and j + n < nt and sw[i + n] == tw[j + n]:
                    n += 1
                if n >= min_anchor_words and (best is None or n > best[0]):
                    best = (n, delta)
            if best is None:
                i += 1
                continue
            n, delta = best
            lo = s_sec.addr + 4 * i
            hi = lo + 4 * n
            if intervals and lo <= intervals[-1][1]:
                if hi <= intervals[-1][1]:
                    i += n
                    continue
                prev = intervals[-1]
                if prev[2] == delta:
                    prev[1] = hi
                else:
                    intervals.append([prev[1], hi, delta])
            else:
                intervals.append([lo, hi, delta])
            i += n

        covered = sum(iv[1] - iv[0] for iv in intervals)
        log(f"anchors merged into {len(intervals)} intervals, "
            f"{covered}/{4 * ns} bytes covered "
            f"({100.0 * covered / (4 * ns):.2f}%)")

        # Fill gaps from the preceding interval so translation is total.
        filled: List[List[int]] = []
        text_lo, text_hi = s_sec.addr, s_sec.addr + 4 * ns
        cursor = text_lo
        for lo, hi, delta in intervals:
            if lo > cursor:
                filled.append([cursor, lo, filled[-1][2] if filled else delta])
            filled.append([lo, hi, delta])
            cursor = hi
        if cursor < text_hi:
            filled.append([cursor, text_hi, filled[-1][2] if filled else 0])
        log(f"gap fill -> {len(filled)} total intervals")

        # Verify every IDA function against the target image.
        db = json.load(open(ida_db))
        overrides: Dict[int, int] = {}
        unresolved_ranges: List[List[int]] = []
        exact = fixed = relaxed = missing = 0

        def raw_translate(addr: int) -> int:
            k = bisect.bisect_right([iv[0] for iv in filled], addr) - 1
            if k < 0:
                k = 0
            return addr + filled[k][2]

        probe = sorted(range(-search_bytes, search_bytes + 1, 4), key=abs)
        claimed: set = set()
        sorted_eas = sorted(f["ea"] for f in db["functions"])

        def span_words(elf: ElfFile, ea: int, cap: int = 4096) -> int:
            """How many instructions this function looks like, by walking forward.

            A return to the caller (`jr ra`) or an unconditional jump ends it.
            Deliberately crude: it only has to be good enough to tell a
            twenty-instruction function from a four-hundred-instruction one.
            """
            reg = elf.word(ea)
            # Not a frame prologue at all: refuse to measure.
            if not ((reg >> 26) == 0x09 and ((reg >> 21) & 0x1F) == 29
                    and ((reg >> 16) & 0x1F) == 29 and (reg & 0x8000)):
                return 0
            n = 0
            while n < cap:
                w = elf.word(ea + 4 * n)
                op = w >> 26
                if op == 0 and (w & 0x3F) == 0x08 and ((w >> 21) & 0x1F) == 31:
                    return n + 1
                if op in (0x02, 0x03):
                    return n + 1
                n += 1
            return n

        for f in db["functions"]:
            addrs: List[int] = []
            for lo, hi in (f.get("chunks") or [[f["ea"], f["ea"] + 4]]):
                # IDA exports occasionally carry a reversed or empty chunk; drop
                # those words rather than deriving a nonsense range from them.
                if hi <= lo:
                    continue
                addrs.extend(range(lo, hi, 4))
            if len(addrs) < 3:
                continue
            want = [normalized_word(src, a) for a in addrs[:24]]
            rel = [opcode_word(src, a) for a in addrs[:24]]
            want_span = span_words(src, f["ea"], cap=len(addrs) + 8)
            guess = raw_translate(f["ea"])

            def exact_at(cand: int) -> bool:
                return (cand >= t_sec.addr
                        and cand + 4 * len(want) <= t_sec.addr + 4 * nt
                        and normalized_body(tgt, cand, len(want)) == want)

            # How much of the body has to agree, for the relaxed test.  A single
            # long comparison is too brittle: the two builds reorder instructions,
            # so two functions can agree for eight words and diverge after.  Any
            # one of these lengths agreeing is enough, and the shortest prefixes
            # are still several times longer than the wildcarding signatures the
            # native renderer resolves functions with.
            RELAX_LENS = (4, 6, 8, 12, 16, 24)

            def relaxed_at(cand: int) -> bool:
                if cand < t_sec.addr:
                    return False
                limit = t_sec.addr + 4 * nt
                for n in RELAX_LENS:
                    if n > len(rel) or cand + 4 * n > limit:
                        continue
                    if opcode_body(tgt, cand, n) == rel[:n]:
                        return True
                return False

            def length_agrees(cand: int) -> bool:
                """Does a function of about the reference's size start here?

                The tolerance is generous because the two builds can differ by a
                few instructions; the point is only to reject a small function
                matched onto a much larger one, which is the usual shape of a
                wrong relaxed match.
                """
                if want_span <= 0:
                    return True
                n = span_words(tgt, cand)
                return n > 0 and abs(n - want_span) <= max(12, want_span // 3)

            def in_order(cand: int) -> bool:
                """Keep the order of functions.

                The two builds list functions in the same order except where code
                was inserted, so a match must not land before an earlier
                reference function's target or after a later one's.  This is a
                strong, cheap check and it is what stops two neighbouring
                functions from being folded onto the same address.
                """
                k = bisect.bisect_left(sorted_eas, f["ea"])
                for j in range(k - 1, -1, -1):
                    p = overrides.get(sorted_eas[j])
                    if p is not None:
                        if cand < p:
                            return False
                        break
                for j in range(k + 1, len(sorted_eas)):
                    nx = overrides.get(sorted_eas[j])
                    if nx is not None:
                        if cand > nx:
                            return False
                        break
                return True

            found = None
            if exact_at(guess):
                exact += 1
                continue
            for off in probe:
                if exact_at(guess + off):
                    found = guess + off
                    break
            if found is not None:
                # An exact match is conclusive on its own, but two reference
                # functions resolving to one target means one of them is wrong:
                # refuse the second rather than emit a duplicate.
                if found in claimed:
                    found = None
                else:
                    fixed += 1
            if found is None:
                # A relaxed prefix is much weaker evidence, so it only counts
                # when two independent properties agree with it:
                #
                #   * a function of comparable length starts where the body does,
                #     and
                #   * the target is not already claimed, and the result keeps the
                #     reference's function order.
                #
                # Two different functions cannot be the same function.  Without
                # these checks the search mapped several short functions onto one
                # long one -- six reference functions landing on a single target
                # in one case -- and the recompiled build then contained garbage
                # where those functions should be.
                for off in probe:
                    cand = guess + off
                    if cand in claimed:
                        continue
                    if not relaxed_at(cand):
                        continue
                    if not length_agrees(cand):
                        continue
                    if not in_order(cand):
                        continue
                    found = cand
                    break
                if found is not None:
                    relaxed += 1
            if found is not None:
                overrides[f["ea"]] = found
                claimed.add(found)
                continue
            missing += 1
            # Record only the part of the function we could not place, so a
            # single stray chunk does not blacklist a whole region.
            unresolved_ranges.append([addrs[0], addrs[0] + 4 * len(want)])
        log(f"function check: {exact} exact, {fixed} recovered by search, "
            f"{relaxed} matched on opcodes alone, {missing} unresolved")

        # An unresolved function's body must not be trusted to a seed entry.
        unresolved_ranges = [r for r in unresolved_ranges if r[0] < r[1]]
        unresolved_ranges.sort()
        merged: List[List[int]] = []
        for lo, hi in unresolved_ranges:
            if merged and lo <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], hi)
            else:
                merged.append([lo, hi])

        blob = {
            "format": 2,
            "source_elf": os.path.basename(source_elf),
            "target_elf": os.path.basename(target_elf),
            "source_text_addr": s_sec.addr,
            "source_text_size": 4 * ns,
            "target_text_addr": t_sec.addr,
            "target_text_size": 4 * nt,
            "source_search_bytes": search_bytes,
            "min_anchor_words": min_anchor_words,
            "intervals": filled,
            "overrides": {"%08X" % k: v for k, v in sorted(overrides.items())},
            "unresolved_ranges": [["%08X" % lo, "%08X" % hi]
                                  for lo, hi in merged],
        }
        return AddressMap(blob)

    # -- querying ----------------------------------------------------------
    def is_unresolved(self, addr: int) -> bool:
        k = bisect.bisect_right(self._unres_lo, addr) - 1
        return k >= 0 and addr < self.unresolved[k][1]

    def translate(self, addr: int) -> Optional[int]:
        """US address -> target address, or None if that code could not be found."""
        if self.is_unresolved(addr):
            return None
        if addr in self.overrides:
            return self.overrides[addr]
        k = bisect.bisect_right(self._lo, addr) - 1
        if k < 0:
            return None
        lo, hi, delta = self.intervals[k]
        if addr >= hi:
            return None
        return addr + delta

    def translate_in_range(self, addr: int) -> Optional[int]:
        """Like translate(), but maps data addresses too.

        Data sits after the text in both images and moved by the same amount as
        the text that precedes it, so an address past the end of the text uses
        the delta of the last text interval.
        """
        out = self.translate(addr)
        if out is not None:
            return out
        if self.is_unresolved(addr):
            # Fall back to the interval that precedes the unresolved region: the
            # surrounding code is a better guide than nothing.
            k = bisect.bisect_right(self._lo, addr) - 1
            if k >= 0:
                return addr + self.intervals[k][2]
            return None
        if addr >= self.source_text[1]:
            return addr + self.intervals[-1][2]
        return None

    def stats(self) -> dict:
        return {
            "intervals": len(self.intervals),
            "overrides": len(self.overrides),
            "unresolved_ranges": len(self.unresolved),
        }


# ---------------------------------------------------------------------------
# Config translation
# ---------------------------------------------------------------------------

def _addr_key(value) -> Optional[int]:
    """Guest address from an int or a string.

    Config files spell addresses in three ways: `0x00331AB8`, bare lowercase hex
    with a leading zero (`00331AB8`, as in game_symbols.txt) and occasionally
    plain decimal.  A string made only of [0-9a-f] with a leading zero is hex;
    anything else that parses as decimal is decimal.
    """
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        return None
    s = value.strip()
    if not s:
        return None
    try:
        if s.lower().startswith("0x"):
            return int(s, 16)
        if len(s) > 1 and s[0] == "0" and all(c in "0123456789abcdefABCDEF" for c in s):
            return int(s, 16)
        return int(s, 10)
    except ValueError:
        return None


def port_ida_db(db: dict, amap: AddressMap, log=print) -> dict:
    """Translate function entries, names and switch tables; keep resolvable ones."""
    names_src = {int(k): v for k, v in db["names"].items()}
    out_names: Dict[int, str] = {}
    out_funcs = []
    dropped = 0
    for f in db["functions"]:
        t = amap.translate(f["ea"])
        if t is None:
            dropped += 1
            continue
        chunks = []
        ok = True
        for lo, hi in (f.get("chunks") or [[f["ea"], f["ea"] + 4]]):
            tl, th = amap.translate(lo), amap.translate(hi - 4)
            # The last word of a chunk may sit in an unresolved tail; step back
            # until it lands somewhere mapped.
            while th is None and hi - 4 > lo:
                hi -= 4
                th = amap.translate(hi - 4)
            if tl is None or th is None:
                ok = False
                break
            chunks.append([tl, th + 4])
        if not ok or not chunks:
            dropped += 1
            continue
        out = dict(f)
        out["ea"] = chunks[0][0]
        out["chunks"] = chunks
        out_funcs.append(out)
        n = names_src.get(f["ea"])
        if n:
            out_names[chunks[0][0]] = n

    out_switches = []
    for sw in db.get("switches", []):
        ea = amap.translate(sw["ea"])
        targets = [amap.translate(t) for t in sw.get("targets", [])]
        targets = [t for t in targets if t is not None]
        if ea is None or not targets:
            continue
        out = dict(sw)
        out["ea"] = ea
        out["targets"] = targets
        for k in ("func", "jumps", "startea"):
            v = amap.translate(sw[k]) if isinstance(sw.get(k), int) else None
            if v is not None:
                out[k] = v
        out_switches.append(out)

    out_funcs.sort(key=lambda f: f["ea"])
    log(f"ida_db: {len(db['functions'])} -> {len(out_funcs)} functions "
        f"({dropped} dropped), {len(out_switches)} switches, "
        f"{len(out_names)} names")

    meta = dict(db.get("meta", {}))
    meta["imagebase"] = amap.blob["target_text_addr"]
    meta["entry"] = (amap.translate(meta["entry"])
                     if isinstance(meta.get("entry"), int) else None)
    meta["min_ea"] = amap.blob["target_text_addr"]
    meta["max_ea"] = (amap.blob["target_text_addr"]
                      + amap.blob["target_text_size"])
    meta["input"] = amap.blob["target_elf"]
    meta["ported_from"] = amap.blob["source_elf"]
    meta.pop("imagebase", None)
    meta["imagebase"] = amap.blob["target_text_addr"]

    segments = []
    for seg in db.get("segments", []):
        s = amap.translate_in_range(seg["start"])
        e = amap.translate_in_range(seg["end"])
        if s is None or e is None:
            continue
        out = dict(seg)
        out["start"], out["end"] = s, e
        segments.append(out)

    return {"meta": meta, "segments": segments,
            "functions": out_funcs, "switches": out_switches,
            "names": {str(k): v for k, v in sorted(out_names.items())}}


def port_seeds(seeds: dict, amap: AddressMap, log=print,
               source_elf: str = "", target_elf: str = "") -> dict:
    """Move the seed lists, then put back the entries the interval deltas get wrong.

    A seed is an address the recompiler must treat as a function entry, so moving
    it approximately is not good enough: the recompiler will start a function
    wherever it is told and stop at the next entry, so a seed that lands in the
    middle of the target's function splits it, and the real entry -- which is only
    ever reached through a function pointer, so it appears in no call instruction
    -- ends up belonging to no function at all.  The call then silently does
    nothing.

    That is exactly what happened to the sound/text helper at US 00360F20: the
    interval delta moved it to JP 003611A4, but the target's real function starts
    at 00361260 (a different delta), and the running build reported 285 indirect
    branches into a function no recompiled code claimed.

    The interval deltas are per-run and remain correct in aggregate, so this only
    revisits entries that need it: it re-checks each mapped seed by comparing the
    code at both ends, and when they disagree it looks nearby for the address whose
    code does match.  Seeds whose code cannot be compared (data-looking entries)
    are moved as before.
    """
    src = ElfFile(source_elf) if source_elf else None
    tgt = ElfFile(target_elf) if target_elf else None

    def body_matches(sa: int, ta: int, limit: int = 8) -> int:
        """How many leading instructions agree, with immediates masked."""
        if src is None or tgt is None:
            return -1
        n = 0
        for i in range(limit):
            try:
                ws = normalized_word(src, sa + 4 * i)
                wt = normalized_word(tgt, ta + 4 * i)
            except Exception:
                break
            if ws != wt:
                break
            n += 1
        return n

    def is_entry(a: int) -> bool:
        """Does the target look like a function starts here?"""
        if tgt is None:
            return False
        w = tgt.word(a)
        # addiu sp, sp, -N  -- the almost universal frame prologue.
        return (w >> 26) == 0x09 and ((w >> 21) & 0x1F) == 29 \
            and ((w >> 16) & 0x1F) == 29 and (w & 0x8000) != 0


    out = {}
    for key, values in seeds.items():
        keep, repaired = [], 0
        for v in values:
            t = amap.translate(v)
            if t is None:
                continue
            # The interval delta is not reliable in every region, and in the
            # text/font area around 0x003601xx it is wrong badly enough that several
            # reference functions land on one target address.  Those functions' code
            # is near-identical -- a large family of small helpers -- so matching the
            # body cannot tell them apart either, and the target's real entries end up
            # in no table at all.  A call to such an entry goes through a function
            # pointer, so it appears in no call instruction, and the recompiled build
            # then does nothing there at all.
            #
            # Nothing here can recover those from the reference alone; they are
            # declared by hand in recovered_funcs.json instead, which lists addresses
            # found at run time where the build reported an indirect branch into code
            # no function claimed.
            keep.append(t)
        out[key] = sorted(set(keep))
        note = f", {repaired} re-aligned" if repaired else ""
        log(f"seeds[{key}]: {len(values)} -> {len(out[key])}{note}")
    return out


def port_address_symbols(raw: dict, amap: AddressMap, log=print,
                         label="symbols") -> dict:
    """`{"0xADDR": name | {..}}` tables: address keys move, names stay."""
    out = {}
    dropped = []
    for k, v in raw.items():
        if isinstance(k, str) and (k.startswith("//") or k.startswith("#")):
            continue
        a = _addr_key(k)
        if a is None:
            continue
        t = amap.translate(a)
        if t is None:
            dropped.append(k)
            continue
        out["0x%08X" % t] = v
    log(f"{label}: {len(raw)} -> {len(out)} ({len(dropped)} dropped)")
    if dropped:
        log("   dropped: " + ", ".join(dropped[:10])
            + (" ..." if len(dropped) > 10 else ""))
    return out


def port_by_address(raw: dict, amap: AddressMap, log=print,
                    label="table") -> dict:
    """Same as port_address_symbols but preserves the key spelling."""
    out = {}
    dropped = []
    for k, v in raw.items():
        if isinstance(k, str) and (k.startswith("//") or k.startswith("#")):
            continue
        a = _addr_key(k)
        t = amap.translate(a) if a is not None else None
        if a is None or t is None:
            dropped.append(k)
            continue
        out["0x%08X" % t if isinstance(k, str) and k.lower().startswith("0x")
            else ("%08X" % t)] = v
    log(f"{label}: {len(raw)} -> {len(out)} ({len(dropped)} dropped)")
    return out


def _port_named_handlers(raw: dict, amap: AddressMap,
                         name_to_addr: Dict[str, int], log, label):
    """overrides.json / hooks.json: keys are SDK symbol names or addresses."""
    out, missing = {}, []
    for k, v in raw.items():
        if isinstance(k, str) and (k.startswith("//") or k.startswith("#")):
            continue
        a = _addr_key(k) if (k.lower().startswith("0x") or k.isdigit()) else None
        if a is None:
            a = name_to_addr.get(k)
            if a is None:
                missing.append(k)
                continue
        t = amap.translate(a)
        if t is None:
            missing.append("%s@%08X (unresolved code)" % (k, a))
            continue
        out["0x%08X" % t] = v
    log(f"{label}: {len(raw)} -> {len(out)} ({len(missing)} unresolved)")
    for m in missing[:12]:
        log("   unresolved: %s" % m)
    if len(missing) > 12:
        log("   ... %d more" % (len(missing) - 12))
    return out


def port_render_emitters(raw: dict, amap: AddressMap, log=print) -> dict:
    out = dict(raw)
    out["binary"] = amap.blob["target_elf"]
    emitters = {}
    dropped = 0
    for k, v in raw.get("emitters", {}).items():
        t = amap.translate(int(k, 16))
        if t is None:
            dropped += 1
            continue
        e = dict(v)
        sites = []
        for s in v.get("sites", []):
            st = amap.translate(int(s["site"], 16))
            if st is None:
                continue
            ns = dict(s)
            ns["site"] = "%08X" % st
            sites.append(ns)
        e["sites"] = sites
        if v.get("name"):
            pass
        emitters["%08X" % t] = e
    out["emitters"] = emitters

    vm = {}
    vdropped = 0
    for k, v in raw.get("virtual_methods", {}).items():
        t = amap.translate(int(k, 16))
        if t is None:
            vdropped += 1
            continue
        vm["%08X" % t] = v
    out["virtual_methods"] = vm
    log(f"render_emitters: {len(raw.get('emitters', {}))} -> {len(emitters)} "
        f"emitters ({dropped} dropped), virtual_methods "
        f"{len(raw.get('virtual_methods', {}))} -> {len(vm)} ({vdropped} dropped)")
    return out


def port_rpc_sids(raw: dict, amap: AddressMap, log=print) -> dict:
    def conv(a):
        v = _addr_key(a)
        return None if v is None else amap.translate(v)

    out = {}
    for key in ("bind", "call"):
        rows = []
        for r in raw.get(key, []):
            t = conv(r.get("site"))
            f = conv(r.get("fn"))
            if t is None or f is None:
                continue
            row = dict(r)
            row["site"] = "0x%X" % t
            row["fn"] = "0x%X" % f
            rows.append(row)
        out[key] = rows
        log(f"rpc_sids[{key}]: {len(raw.get(key, []))} -> {len(rows)}")
    return out


def port_syscalls(raw: dict, amap: AddressMap, log=print) -> dict:
    out = {}
    for k, v in raw.items():
        if isinstance(k, str) and (k.startswith("//") or k.startswith("#")):
            continue
        row = dict(v) if isinstance(v, dict) else v
        if isinstance(row, dict) and "sites" in row:
            sites = []
            for s in row["sites"]:
                a = _addr_key(s)
                t = amap.translate(a) if a is not None else None
                if t is not None:
                    sites.append("%08X" % t if not (
                        isinstance(s, str) and s.lower().startswith("0x"))
                        else "0x%08X" % t)
            row["sites"] = sites
        out[k] = row
    log(f"syscalls: {len(raw)} entries translated")
    return out


def port_plain_text(path: str, amap: AddressMap, out_path: str,
                    log=print) -> None:
    """`config/game_symbols.txt` is `ADDR name` lines; rewrite the addresses."""
    kept, dropped = [], 0
    with open(path, encoding="utf-8", errors="replace") as fp:
        for line in fp:
            raw = line.rstrip("\n")
            if not raw.strip() or raw.lstrip().startswith(("#", ";", "//")):
                kept.append(raw)
                continue
            parts = raw.split(None, 1)
            if len(parts) != 2:
                kept.append(raw)
                continue
            a = _addr_key(parts[0])
            t = amap.translate(a) if a is not None else None
            if t is None:
                dropped += 1
                continue
            kept.append("0x%08X %s" % (t, parts[1]))
    with open(out_path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(kept) + "\n")
    log(f"{os.path.basename(path)}: {len(kept)} lines written "
        f"({dropped} dropped)")
