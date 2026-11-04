# LE (DOS/4GW) -> flat x86 image at base 0x10000 with internal fixups applied.
# shortcut: only internal-reference fixups (no imports, which DOS/4GW apps lack).
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
le = struct.unpack_from('<I', d, 0x3c)[0]
H = lambda o: struct.unpack_from('<I', d, le + o)[0]
npages, psz, last = H(0x14), H(0x28), H(0x2c)
nobj, objtab, pmap = H(0x44), le + H(0x40), le + H(0x48)
fpt, frt, dpages = le + H(0x68), le + H(0x6c), H(0x80)
objs = [struct.unpack_from('<6I', d, objtab + i * 24) for i in range(nobj)]
BASE = 0x10000
end = max(o[1] + max(o[0], o[4] * psz) for o in objs)
img = bytearray(end - BASE)

def page_file(p):  # 1-based logical page -> (file offset, size)
    e = d[pmap + (p - 1) * 4: pmap + p * 4]
    num = (e[0] << 16) | (e[1] << 8) | e[2]
    return dpages + (num - 1) * psz, (last if num == npages else psz)

for vs, base, fl, pi, pn, _ in objs:
    for k in range(pn):
        off, sz = page_file(pi + k)
        a = base - BASE + k * psz
        img[a:a + sz] = d[off:off + sz]

nfix = 0
for vs, base, fl, pi, pn, _ in objs:
    for k in range(pn):
        p = pi + k
        r = frt + struct.unpack_from('<I', d, fpt + (p - 1) * 4)[0]
        rend = frt + struct.unpack_from('<I', d, fpt + p * 4)[0]
        page_va = base + k * psz
        while r < rend:
            s, f = d[r], d[r + 1]; r += 2
            assert f & 3 == 0, 'non-internal fixup %x at page %d' % (f, p)
            lst = s & 0x20  # 0x10 is the alias flag
            if lst: cnt = d[r]; r += 1
            else: so = struct.unpack_from('<h', d, r)[0]; r += 2
            if f & 0x40: obj = struct.unpack_from('<H', d, r)[0]; r += 2
            else: obj = d[r]; r += 1
            t = s & 0xf
            toff = 0
            if t != 2:
                if f & 0x10: toff = struct.unpack_from('<I', d, r)[0]; r += 4
                else: toff = struct.unpack_from('<H', d, r)[0]; r += 2
            sos = [struct.unpack_from('<h', d, r + 2 * i)[0] for i in range(cnt)] if lst else [so]
            if lst: r += 2 * cnt
            assert 1 <= obj <= nobj, "bad fixup object %d at page %d" % (obj, p)
            tgt = objs[obj - 1][1] + toff
            for so in sos:
                a = page_va + so - BASE
                if a < 0 or a + 4 > len(img): continue
                if t == 7: struct.pack_into('<I', img, a, tgt)
                elif t == 8: struct.pack_into('<i', img, a, tgt - (page_va + so + 4))
                elif t == 6: struct.pack_into('<I', img, a, tgt)  # 16:32, selector left as is
                elif t in (3, 5): struct.pack_into('<H', img, a, tgt & 0xffff)
                nfix += 1
open(dst, 'wb').write(img)
eip = objs[H(0x18) - 1][1] + H(0x1c)
print('image', hex(BASE), '-', hex(end), 'fixups', nfix, 'entry', hex(eip))
