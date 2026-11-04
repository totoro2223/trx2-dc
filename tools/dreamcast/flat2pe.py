# Wrap the flat image (VA 0x10000) in a minimal PE32 so Ghidra keeps the LE addresses.
import struct, sys
flat, dst, entry = open(sys.argv[1], 'rb').read(), sys.argv[2], int(sys.argv[3], 16)
BASE, FA = 0x10000, 0x200
secs = [(b'.text', 0x10000, 0xb1000, 0x60000020), (b'.data', 0xc0000, BASE + len(flat), 0xC0000040)]
hdr_sz = 0x400
out = bytearray(hdr_sz)
out[0:2] = b'MZ'; struct.pack_into('<I', out, 0x3c, 0x80)
p = 0x80
out[p:p + 4] = b'PE\0\0'
struct.pack_into('<HHIIIHH', out, p + 4, 0x14c, len(secs), 0, 0, 0, 0xe0, 0x010f)
o = p + 24
struct.pack_into('<HBBIIIIII', out, o, 0x10b, 1, 0, 0, 0, 0, entry, 0x10000, 0xc0000)
struct.pack_into('<IIIHHHHHHIIIIHHIIIIII', out, o + 28, 0, 0x1000, FA, 4, 0, 0, 0, 4, 0, 0,
                 BASE + len(flat), hdr_sz, 0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
s = o + 0xe0
raw = hdr_sz
body = bytearray()
for i, (name, lo, hi, ch) in enumerate(secs):
    data = flat[lo - BASE:hi - BASE]
    data += b'\0' * (-len(data) % FA)
    struct.pack_into('<8sIIIIIIHHI', out, s + i * 40, name, hi - lo, lo, len(data), raw + len(body), 0, 0, 0, 0, ch)
    body += data
open(dst, 'wb').write(out + body)
print('pe', dst, 'size', len(out) + len(body))
