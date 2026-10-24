#!/usr/bin/env python3
"""Patch a streaming LZMA uImage so U-Boot's LZMA decoder accepts it.

The kernel's lzma encoder writes the "unknown size" marker when its output is
a pipe.  U-Boot 1.1.3's lzmaBuffToBuffDecompress() rejects that with
outSize == 0xffffffff and needs the real uncompressed size in the 13-byte
LZMA-alone header.  The compressed stream is valid, so only the size field
and the two uImage CRCs need updating.
"""

import struct
import subprocess
import sys
import zlib

try:
	import lzma
except ImportError:
	lzma = None

UIMAGE_MAGIC = b"\x27\x05\x19\x56"
IH_COMP_LZMA = 3
UNKNOWN_SIZE = 0xFFFFFFFFFFFFFFFF


def raw_length(payload):
	if lzma is not None:
		return len(lzma.decompress(payload, format=lzma.FORMAT_ALONE))
	proc = subprocess.run(
		["xz", "--format=lzma", "--decompress", "--stdout"],
		input=payload, stdout=subprocess.PIPE, check=True)
	return len(proc.stdout)


def main(path):
	data = bytearray(open(path, "rb").read())
	if len(data) < 64 or data[:4] != UIMAGE_MAGIC:
		return 0
	if data[31] != IH_COMP_LZMA:
		return 0

	size = struct.unpack(">I", data[12:16])[0]
	offset = 64
	payload = bytes(data[offset:offset + size])
	if len(payload) < 13 or payload[0] >= 9 * 5 * 5:
		return 0

	declared = int.from_bytes(payload[5:13], "little")
	if declared != UNKNOWN_SIZE:
		return 0

	length = raw_length(payload)
	data[offset + 5:offset + 13] = length.to_bytes(8, "little")

	dcrc = zlib.crc32(bytes(data[offset:offset + size])) & 0xFFFFFFFF
	data[24:28] = struct.pack(">I", dcrc)

	data[4:8] = b"\0\0\0\0"
	hcrc = zlib.crc32(bytes(data[:64])) & 0xFFFFFFFF
	data[4:8] = struct.pack(">I", hcrc)

	open(path, "wb").write(data)
	print("fixed LZMA size field in %s (%d bytes)" % (path, length))
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1]))
