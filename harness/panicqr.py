#!/usr/bin/env python3
"""Decode a Linux drm_panic QR code into the kernel log it carries.

The panic screen's QR code is a URL (e.g. panic.archlinux.org/panic_report#?
a=...&v=...&z=...). z is the zlib-compressed kmsg tail, written as decimal
digits so the QR code can use numeric mode: every 7 bytes become 17 digits,
little-endian, and a shorter final group uses the fewest digits that fit.

Read the code from a photo first, e.g. with zxing-cpp (it copes with
version-40 codes shot at an angle; zbar often doesn't):
    ZXingReader -fast -format QRCode photo.png
usage: panicqr.py URL-or-file
"""
import os
import re
import sys
import zlib

arg = sys.argv[1]
text = open(arg).read() if os.path.exists(arg) else arg
m = re.search(r"[?&#]z=(\d+)", text)
if not m:
    sys.exit("no z= parameter found")
digits = m.group(1)
# bytes -> digits: ceil(n * 8 * log10(2))
nbytes = {3: 1, 5: 2, 8: 3, 10: 4, 13: 5, 15: 6, 17: 7}
data = bytearray()
for i in range(0, len(digits), 17):
    chunk = digits[i:i + 17]
    if len(chunk) not in nbytes:
        sys.exit(f"bad final group of {len(chunk)} digits")
    data += int(chunk).to_bytes(nbytes[len(chunk)], "little")
for k in ("a", "v"):
    if (p := re.search(rf"[?&#]{k}=([^&]+)", text)):
        print(f"# {k}={p.group(1)}")
sys.stdout.write(zlib.decompress(bytes(data)).decode("utf-8", "replace"))
