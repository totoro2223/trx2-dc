#!/bin/bash
# Turns a staged disc folder (make_disc_all.sh's disc-all) into a GDI:
# track01 data + track02 audio (silence) + track03 data at LBA 45000.
#   tools/dreamcast/stage_to_gdi.sh <stage-dir> <out-dir>
# shortcut: EDC/ECC left at zero (GDEMU/DreamShell/Flycast do not check them).
source /opt/toolchains/dc/kos/environ.sh
set -eo pipefail
STAGE="$(cd "${1:?stage dir}" && pwd)"; OUT="${2:?out dir}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
T="$(mktemp -d)"
/opt/toolchains/dc/kos/utils/makeip/makeip -f -g "TRX" -c "TRX PORT" -n "T-TRX00" "$T/IP.BIN" > /dev/null
bash "$(dirname "$0")/disc_sort.sh" "$STAGE" > "$T/sort.txt"
mkisofs -quiet -C 0,45000 -V TRX -G "$T/IP.BIN" -r -J -l -sort "$T/sort.txt" -o "$T/t3.iso" "$STAGE"
python - "$T/t3.iso" "$OUT" <<'PY'
import sys
src, out = sys.argv[1], sys.argv[2]
def raw(lba, data):  # mode 1 sector: sync, MSF header, 2048 data, zero EDC/ECC
    f = lba + 150
    b = lambda v: (v // 10) << 4 | v % 10
    return b'\x00' + b'\xff' * 10 + b'\x00' + bytes([b(f // 4500), b(f // 75 % 60), b(f % 75), 1]) + data + bytes(288)
with open(out + '/track01.bin', 'wb') as f:
    for i in range(300): f.write(raw(i, bytes(2048)))
with open(out + '/track02.raw', 'wb') as f:
    f.write(bytes(2352 * 300))
n = 0
with open(src, 'rb') as s, open(out + '/track03.bin', 'wb') as d:
    while (c := s.read(2048 * 512)):
        c = c.ljust((len(c) + 2047) // 2048 * 2048, b'\0')
        for i in range(0, len(c), 2048):
            d.write(raw(45000 + n, c[i:i + 2048])); n += 1
open(out + '/disc.gdi', 'w', newline='\r\n').write(
    "3\n1 0 4 2352 track01.bin 0\n2 756 0 2352 track02.raw 0\n3 45000 4 2352 track03.bin 0\n")
print("sectors", n)
PY
rm -rf "$T"; ls -la "$OUT"
