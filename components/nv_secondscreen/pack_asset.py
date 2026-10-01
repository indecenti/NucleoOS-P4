"""Build step: raw-deflate a text asset for ss_assets.cpp (4-byte little-endian length + stream).

    python pack_asset.py <in> <out>
"""
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
c = zlib.compressobj(9, zlib.DEFLATED, -15)
z = c.compress(data) + c.flush()
open(dst, 'wb').write(struct.pack('<I', len(data)) + z)
