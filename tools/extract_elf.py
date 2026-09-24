"""Pull the game executable out of a PS2 disc image.

Only ISO9660 is handled, which is what both Ace Combat 5 discs use.  This exists
so the build does not depend on 7-Zip or a mounted image.

    python tools/extract_elf.py <disc.iso> <name-in-root> <output-path>
    python tools/extract_elf.py <disc.iso>              # just list the root

The primary volume descriptor is located through the ISO9660 system area, so an
ISO with a different sector size in the descriptor still reads correctly.
"""

from __future__ import annotations

import struct
import sys
import os

SECTOR = 2048
SYSTEM_AREA = 16 * SECTOR


class IsoError(Exception):
    pass


def _both32(b: bytes) -> int:
    """ISO9660 stores many numbers twice (little endian then big endian)."""
    le, be = struct.unpack_from("<I", b, 0)[0], struct.unpack_from(">I", b, 4)[0]
    if le != be:
        raise IsoError("ISO9660 endian mismatch in directory record")
    return le


def find_pvd(fp):
    """Return (sector_size, root_record_bytes) from the primary volume descriptor."""
    offset = SYSTEM_AREA
    while True:
        fp.seek(offset)
        vd = fp.read(SECTOR)
        if len(vd) < SECTOR:
            raise IsoError("no primary volume descriptor found")
        vtype = vd[0]
        ident = vd[1:6]
        if ident != b"CD001":
            # Not a descriptor we understand; some images pad with zeroes.
            if vtype == 0 and ident == b"\0\0\0\0\0":
                raise IsoError("this does not look like an ISO9660 image")
            raise IsoError("bad volume descriptor identifier %r" % ident)
        if vtype == 1:                       # primary volume descriptor
            logical_block = struct.unpack_from("<H", vd, 128)[0]
            block_size = logical_block or SECTOR
            root = vd[156:156 + 34]
            return block_size, root
        if vtype == 255:                     # terminator
            raise IsoError("no primary volume descriptor before the terminator")
        offset += SECTOR


def read_dir(fp, lba, size):
    """Parse one directory extent into {name: (lba, size, is_dir)}."""
    block = 2048
    out = {}
    pos = 0
    fp.seek(lba * block)
    data = fp.read(size)
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            # Move to the next logical block boundary.
            pos = (pos // block + 1) * block
            continue
        rec = data[pos:pos + rec_len]
        ext_lba = _both32(rec[2:10])
        ext_size = _both32(rec[10:18])
        flags = rec[25]
        name_len = rec[32]
        raw = rec[33:33 + name_len]
        if name_len == 1 and raw in (b"\0", b"\1"):
            pos += rec_len
            continue
        name = raw.decode("latin1")
        if name.endswith(";1"):
            name = name[:-2]
        out[name] = (ext_lba, ext_size, bool(flags & 0x02))
        pos += rec_len
    return out


def list_root(fp):
    block, root = find_pvd(fp)
    lba = _both32(root[2:10])
    size = _both32(root[10:18])
    return block, read_dir(fp, lba, size)


def extract(iso_path, name, out_path):
    with open(iso_path, "rb") as fp:
        block, entries = list_root(fp)
        # The root listing itself is under the "." entry.
        want = name.upper()
        hit = None
        for k, v in entries.items():
            if k.upper() == want or k.upper().split(".")[0] == want.split(".")[0]:
                hit = (k, v)
                break
        if hit is None:
            raise IsoError("%s is not in the image; root contains: %s"
                           % (name, ", ".join(sorted(entries))[:400]))
        _k, (lba, size, is_dir) = hit
        if is_dir:
            raise IsoError("%s is a directory, not the game executable" % name)
        fp.seek(lba * block)
        data = fp.read(size)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as out:
        out.write(data)
    print("extracted %s (%d bytes) -> %s" % (name, len(data), out_path))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    iso = argv[1]
    if not os.path.exists(iso):
        print("no such image: %s" % iso, file=sys.stderr)
        return 1
    try:
        if len(argv) == 2:
            block, entries = list_root(open(iso, "rb"))
            print("%s: block size %d, %d root entries" % (iso, block, len(entries)))
            for name in sorted(entries):
                lba, size, is_dir = entries[name]
                print("  %-32s %10d %s" % (name, size, "dir" if is_dir else ""))
            return 0
        if len(argv) != 4:
            print(__doc__)
            return 2
        return extract(iso, argv[2], argv[3])
    except IsoError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
