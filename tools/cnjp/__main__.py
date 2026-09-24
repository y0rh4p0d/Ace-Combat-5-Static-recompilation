"""Port the Ace Combat 5 US configuration to the Japanese / Chinese build.

    python -m cnjp map  --source SLUS_208.51 --target SLPS_254.18 \
                        --ida-db config/ida_db.json -o config/cnjp/addr_map.json
    python -m cnjp port --map config/cnjp/addr_map.json \
                        --config config --out config/cnjp --region cnjp
"""

from __future__ import annotations

import argparse
import json
import os
import sys

from .port import (AddressMap, port_address_symbols, port_by_address,
                   port_ida_db, port_plain_text,
                   port_render_emitters, port_rpc_sids, port_seeds,
                   port_syscalls)

# Config files that are pure address -> payload tables.
ADDRESS_TABLES = {
    "sdk_symbols.json": "sdk_symbols",
    "manual_symbols.json": "manual_symbols",
    "overrides.json": "overrides",
    "hooks.json": "hooks",
}


def _load(path):
    with open(path, encoding="utf-8") as fp:
        return json.load(fp)


def _save(path, obj):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        json.dump(obj, fp, indent=1, ensure_ascii=False)
        fp.write("\n")


def _port_handlers(raw, amap, name_to_addr, log, label):
    """overrides.json / hooks.json entries: `symbol-or-address -> handler`."""
    from .port import _addr_key
    out, unresolved = {}, []
    for k, v in raw.items():
        if isinstance(k, str) and (k.startswith("//") or k.startswith("#")):
            continue
        looks_numeric = (k.lower().startswith("0x") or k.isdigit()
                         or (len(k) > 1 and k[0] == "0"))
        a = _addr_key(k) if looks_numeric else name_to_addr.get(k)
        if a is None:
            unresolved.append("%s (no address)" % k)
            continue
        t = amap.translate(a)
        if t is None:
            unresolved.append("%s@%08X (code not located in target)" % (k, a))
            continue
        out["0x%08X" % t] = v
    log("%s: %d -> %d (%d unresolved)" % (label, len(raw), len(out),
                                          len(unresolved)))
    for u in unresolved[:15]:
        log("   unresolved: %s" % u)
    if len(unresolved) > 15:
        log("   ... %d more" % (len(unresolved) - 15))
    return out, unresolved


def cmd_map(args):
    log = print if not args.quiet else (lambda *a: None)
    amap = AddressMap.build(args.source, args.target, args.ida_db,
                            search_bytes=args.search, log=log)
    _save(args.out, amap.blob)
    amap.blob.setdefault("_stats", {})
    log(f"wrote {args.out}  {amap.stats()}")
    return 0


def cmd_port(args):
    log = print if not args.quiet else (lambda *a: None)
    amap = AddressMap(_load(args.map))
    cfg = args.config
    out = args.out
    os.makedirs(out, exist_ok=True)
    log(f"address map: {amap.stats()}")

    # ---- ida_db.json -----------------------------------------------------
    ida_src = _load(os.path.join(cfg, "ida_db.json"))
    ida = port_ida_db(ida_src, amap, log)

    # Function entries the map cannot place, declared by hand because the build was
    # seen calling them and no derivation finds them (see the file's own comment).
    recovered = os.path.join(out, "recovered_funcs.json")
    if os.path.exists(recovered):
        rec = {k: v for k, v in _load(recovered).items()
               if not k.startswith("_") and isinstance(v, dict)}
        known = {f["ea"] for f in ida["functions"]}
        added = []
        for tgt_s, spec in sorted(rec.items()):
            t = int(tgt_s, 16)
            if t in known:
                continue
            ref = int(spec["ref"], 16)
            ida["functions"].append({
                "ea": t, "name": "sub_%X" % t,
                "chunks": [[t, t + 4]],
                "recovered_from": "0x%08X" % ref,
                "why": spec.get("why", ""),
            })
            added.append(t)
        if added:
            log("recovered_funcs: +%d function(s): %s"
                % (len(added), " ".join("%08X" % a for a in added)))
    _save(os.path.join(out, "ida_db.json"), ida)

    seeds = port_seeds(_load(os.path.join(cfg, "ida_seeds.json")), amap, log,
                       source_elf=args.source or "", target_elf=args.target or "")
    # A recovered entry must also be seeded, or the recompiler will not create it
    # even though the table lists it: the table says what the target's functions are,
    # the seeds say which addresses to start emitting from.
    if os.path.exists(recovered):
        added = []
        for tgt_s in sorted(rec):
            t = int(tgt_s, 16)
            if not any(t in seeds[k] for k in seeds):
                # flow is the broadest list and is unioned into the seed set.
                seeds.setdefault("flow", []).append(t)
                added.append(t)
        for key in seeds:
            seeds[key] = sorted(set(seeds[key]))
        if added:
            log("recovered_funcs: seeded %s"
                % " ".join("%08X" % a for a in sorted(added)))
    _save(os.path.join(out, "ida_seeds.json"), seeds)

    # ---- symbol tables ---------------------------------------------------
    sdk = port_address_symbols(_load(os.path.join(cfg, "sdk_symbols.json")),
                               amap, log, "sdk_symbols")
    _save(os.path.join(out, "sdk_symbols.json"), sdk)
    manual = port_address_symbols(
        _load(os.path.join(cfg, "manual_symbols.json")), amap, log,
        "manual_symbols")
    _save(os.path.join(out, "manual_symbols.json"), manual)

    # ---- overrides / hooks ----------------------------------------------
    # These are keyed by SDK symbol name, which is stable across builds, so the
    # address has to be recovered from the US symbol tables before it can be
    # translated.  The recompiler resolves numeric keys by address, so emitting
    # translated addresses keeps the entries unambiguous.
    name_to_addr = {}
    for path in ("sdk_symbols.json", "manual_symbols.json"):
        full = os.path.join(cfg, path)
        if not os.path.exists(full):
            continue
        for addr_hex, v in _load(full).items():
            name = v["name"] if isinstance(v, dict) else v
            if name:
                name_to_addr.setdefault(name, int(addr_hex, 16))

    over, over_unresolved = _port_handlers(
        _load(os.path.join(cfg, "overrides.json")), amap, name_to_addr, log,
        "overrides")
    _save(os.path.join(out, "overrides.json"), over)
    hooks, hooks_unresolved = _port_handlers(
        _load(os.path.join(cfg, "hooks.json")), amap, name_to_addr, log,
        "hooks")
    _save(os.path.join(out, "hooks.json"), hooks)

    # ---- renderer tables -------------------------------------------------
    if os.path.exists(os.path.join(cfg, "render_emitters.json")):
        re_ = port_render_emitters(_load(os.path.join(cfg,
                                                      "render_emitters.json")),
                                   amap, log)
        _save(os.path.join(out, "render_emitters.json"), re_)
    if os.path.exists(os.path.join(cfg, "rpc_sids.json")):
        rpc = port_rpc_sids(_load(os.path.join(cfg, "rpc_sids.json")), amap, log)
        _save(os.path.join(out, "rpc_sids.json"), rpc)
    if os.path.exists(os.path.join(cfg, "ac5_syscalls_used.json")):
        sc = port_syscalls(_load(os.path.join(cfg, "ac5_syscalls_used.json")),
                           amap, log)
        _save(os.path.join(out, "ac5_syscalls_used.json"), sc)

    # ---- plain text ------------------------------------------------------
    gs = os.path.join(cfg, "game_symbols.txt")
    if os.path.exists(gs):
        port_plain_text(gs, amap, os.path.join(out, "game_symbols.txt"), log)

    # The remaining files carry no addresses, or are regenerated by a recompile:
    #  - pac_names.txt keys DATA.PAC members by name, which is the same in every
    #    region, so the recompiled build reads the US copy directly.
    #  - report.json is the recompiler's own analysis output and is written
    #    fresh on each run; translating the US one would be stale the moment the
    #    other executable is recompiled.
    note = os.path.join(out, "README.md")
    with open(note, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(
            "# %s configuration\n\n"
            "Generated by `python -m cnjp port` from the US `config/` tree by\n"
            "translating guest addresses to the `%s` executable.\n\n"
            "Address map summary: %d intervals, %d explicit overrides,\n"
            "%d unresolved code ranges.\n\n"
            "`pac_names.txt` is deliberately absent: it names members inside\n"
            "`DATA.PAC` by name, and those names are the same in every region,\n"
            "so the recompiled build reads `config/pac_names.txt` directly.\n\n"
            "`recovered_funcs.json` is the exception to the rule above: it is\n"
            "written by hand, not generated.  It lists function entries in this\n"
            "executable that the address map cannot place, found at run time when\n"
            "the build reported an indirect branch into code no function claimed.\n"
            "The file itself explains each entry and the evidence for it.  The\n"
            "port reads it if present and leaves it alone.\n"
            % (args.region, amap.blob["target_elf"], len(amap.intervals),
               len(amap.overrides), len(amap.unresolved)))
    log(f"wrote {out}/")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="cnjp", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("map", help="derive the address map for a target build")
    m.add_argument("--source", required=True, help="reference ELF (US)")
    m.add_argument("--target", required=True, help="ELF to map onto")
    m.add_argument("--ida-db", required=True, help="reference config/ida_db.json")
    m.add_argument("-o", "--out", required=True)
    m.add_argument("--search", type=int, default=0x400,
                   help="window in bytes to search for a moved function body")
    m.add_argument("-q", "--quiet", action="store_true")
    m.set_defaults(func=cmd_map)

    p = sub.add_parser("port", help="translate config files using an address map")
    p.add_argument("--map", required=True)
    p.add_argument("--config", required=True, help="source config directory")
    p.add_argument("--out", required=True, help="destination config directory")
    p.add_argument("--region", default="cnjp", help="label recorded in the output")
    # The map records only the input file names, so give the port the real paths
    # when the seed realignment needs to compare the two executables' code.
    p.add_argument("--source", help="reference ELF, for seed realignment")
    p.add_argument("--target", help="target ELF, for seed realignment")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(func=cmd_port)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
