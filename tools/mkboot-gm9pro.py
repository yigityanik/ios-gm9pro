#!/usr/bin/env python3
"""Replace the kernel in a GM9 Pro stock v0/v1 image, keeping its DTBs."""
import gzip
import hashlib
from pathlib import Path
import struct
import sys
import zlib


def repack(stock, image):
    if stock[:8] != b"ANDROID!":
        raise ValueError("Not an Android boot image")
    ksize, _, rsize, _, ssize, _, _, page, version = struct.unpack_from("<9I", stock, 8)
    if version not in (0, 1) or page not in (2048, 4096, 8192, 16384):
        raise ValueError("Expected stock Android boot header v0/v1")
    align = lambda n: (n + page - 1) // page * page
    kernel = stock[page:page + ksize]
    dec = zlib.decompressobj(31)
    dec.decompress(kernel)
    if not dec.eof or not dec.unused_data.startswith(b"\xd0\x0d\xfe\xed"):
        raise ValueError("Expected a gzip kernel followed by stock DTBs")
    new_kernel = gzip.compress(image, mtime=0) + dec.unused_data
    rstart = page + align(ksize)
    sstart = rstart + align(rsize)
    ramdisk = stock[rstart:rstart + rsize]
    second = stock[sstart:sstart + ssize]
    if len(ramdisk) != rsize or len(second) != ssize:
        raise ValueError("Truncated boot image")
    header = bytearray(stock[:page])
    struct.pack_into("<I", header, 8, len(new_kernel))
    parts = [new_kernel, ramdisk, second]
    if version == 1:
        dt_size, dt_offset = struct.unpack_from("<IQ", stock, 1632)
        dt = stock[dt_offset:dt_offset + dt_size]
        if len(dt) != dt_size:
            raise ValueError("Truncated recovery DTBO")
        struct.pack_into("<Q", header, 1636,
                         page + sum(align(len(p)) for p in parts) if dt_size else 0)
        parts.append(dt)
    digest = hashlib.sha1()
    for part in parts:
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    header[576:608] = digest.digest().ljust(32, b"\0")
    return bytes(header) + b"".join(p.ljust(align(len(p)), b"\0") for p in parts)


if __name__ == "__main__":
    stock, image, output = map(Path, sys.argv[1:])
    output.write_bytes(repack(stock.read_bytes(), image.read_bytes()))
    print(f"{output}: {output.stat().st_size} bytes")
