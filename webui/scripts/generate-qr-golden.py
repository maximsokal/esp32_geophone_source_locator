"""Generate golden QR matrices with the reference encoder `segno` (dev tool only).

MAX-150. segno is NOT a project dependency. To regenerate the fixture:

    python -m pip download segno==1.6.6 --no-deps -d /tmp/segno
    python -m pip install --target /tmp/segno-lib --no-deps /tmp/segno/segno-1.6.6-py3-none-any.whl
    python scripts/generate-qr-golden.py /tmp/segno-lib tests/console/fixtures/qr_golden.json

then pack the rows to hex as tests/console/qrCode.test.ts expects (one hex
string per mask, row-major bits).
"""
import json
import sys

sys.path.insert(0, sys.argv[1])
import segno  # noqa: E402
from segno import encoder, consts  # noqa: E402

# segno 1.6.6 adds `8 - len % 8` padding bits unconditionally, i.e. a whole
# extra 0x00 codeword when the stream is already byte aligned (always the
# case in byte mode after the 4-bit terminator). ISO/IEC 18004 7.4.10 adds
# padding bits only when the stream does NOT end on a codeword boundary.
# Both decode identically; the golden vectors follow the standard.
def _spec_padding_bits(buff, version, length):
    if version not in (consts.VERSION_M1, consts.VERSION_M3) and length % 8:
        buff.extend([0] * (8 - (length % 8)))
encoder.write_padding_bits = _spec_padding_bits

TEXTS = [
    "https://www.openstreetmap.org/?mlat=50.450100&mlon=30.523400#map=18/50.450100/30.523400",
    "https://www.google.com/maps?q=50.450100,30.523400",
    "HELLO",
    "Geophone report OBJ_0001 / S000001",
    "x" * 180,  # version 9
    "y" * 200,  # version 10 (16-bit count)
]

out = []
for text in TEXTS:
    ref = segno.make_qr(text, error="m", mode="byte", boost_error=False, eci=False)
    version = ref.version
    masks = []
    for mask in range(8):
        q = segno.make_qr(text, error="m", mode="byte", boost_error=False, eci=False, version=version, mask=mask)
        rows = ["".join("1" if m else "0" for m in row) for row in q.matrix]
        masks.append(rows)
    out.append({"text": text, "version": version, "masks": masks})

json.dump(out, open(sys.argv[2], "w", encoding="utf-8"))
print("ok", [(o["version"], len(o["masks"][0])) for o in out])
