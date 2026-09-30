#!/usr/bin/env python3
"""List / extract PopCap .pak archives (PvZ main.pak).

Format (see SexyAppFramework/paklib/PakInterface.cpp):
  u32 magic      = 0xBAC04AC0
  u32 version    = 0
  records:
     u8  flags        (bit FILEFLAGS_END terminates)
     u8  nameWidth
     u8  name[nameWidth]   ('\\' normalized to '/')
     i32 srcSize
     i64 fileTime
  data area starts right after the directory; each record's payload is the raw
  file bytes XOR 0xF7.
"""
import sys, os, struct, collections

MAGIC = 0xBAC04AC0
FILEFLAGS_END = 0x80


def read_records(path):
    with open(path, "rb") as f:
        enc = f.read()
    # PakInterface XORs the whole collection buffer with 0xF7 before parsing
    # (including the header), so decrypt up-front.
    raw = bytes(b ^ 0xF7 for b in enc)
    magic, version = struct.unpack_from("<II", raw, 0)
    if magic != MAGIC:
        raise SystemExit(f"bad magic 0x{magic:08X} (expected 0x{MAGIC:08X})")
    off = 8
    recs = []
    while True:
        if off >= len(raw):
            break
        flags = raw[off]; off += 1
        if flags & FILEFLAGS_END:
            break
        nw = raw[off]; off += 1
        name = raw[off:off + nw].decode("latin-1").replace("\\", "/"); off += nw
        (size,) = struct.unpack_from("<i", raw, off); off += 4
        (ftime,) = struct.unpack_from("<q", raw, off); off += 8
        recs.append([name, size, ftime])
    data_start = off
    pos = data_start
    for r in recs:
        r.append(pos)
        pos += r[1]
    return raw, version, data_start, recs


def payload(raw, rec):
    """raw is already decrypted, so payload bytes are raw bytes."""
    name, size, ftime, pos = rec
    return raw[pos:pos + size]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    pak = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "list"
    raw, version, data_start, recs = read_records(pak)

    if mode == "list":
        print(f"{pak}: version={version} files={len(recs)} data_start={data_start}")
        ext = collections.Counter()
        for name, size, ftime, pos in recs:
            ext[os.path.splitext(name)[1].lower()] += 1
        print("\n-- extensions --")
        for e, c in ext.most_common():
            print(f"  {e or '(none)':10} {c}")
        print("\n-- audio files --")
        for name, size, ftime, pos in recs:
            if os.path.splitext(name)[1].lower() in (
                    ".ogg", ".mp3", ".wav", ".mo3", ".xm", ".mod", ".s3m", ".it", ".flac"):
                print(f"  {size:>9}  {name}")
        print("\n-- all files --")
        for name, size, ftime, pos in recs:
            print(f"  {size:>9}  {name}")
    elif mode == "extract":
        outdir = sys.argv[3]
        for rec in recs:
            dest = os.path.join(outdir, rec[0])
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(payload(raw, rec))
        print(f"extracted {len(recs)} files to {outdir}")
    elif mode == "cat":
        want = sys.argv[3].upper().replace("\\", "/")
        for rec in recs:
            if rec[0].upper() == want:
                sys.stdout.buffer.write(payload(raw, rec))
                return
        raise SystemExit(f"not found: {want}")
    else:
        raise SystemExit(f"unknown mode {mode}")


if __name__ == "__main__":
    main()
