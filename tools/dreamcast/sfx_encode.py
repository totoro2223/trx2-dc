#!/usr/bin/env python3
"""Re-encodes a main.sfx for the Dreamcast and writes its index.

Every WAV of the remaster's main.sfx is PCM; audio.c turned each into the
AICA's 4-bit Yamaha ADPCM at every level load. Done here once instead: the
file is a quarter the size, and the console only copies to sound RAM.

Each sample stays a 44-byte-header WAV (the level loader walks them as such),
with format 0x20 (Yamaha ADPCM), mono, 4 bits; the sample count, which the data
size cannot say exactly, is in the (otherwise unused) byte-rate field. Samples
longer than the AICA counts (65534) are halved in rate, as audio.c did. Other
formats are copied as they are and audio.c still converts them.

The index, <out>.idx, holds the byte offset of every WAV (uint32 LE).

  sfx_encode.py <in main.sfx> <out main.sfx>
"""
import struct
import sys

MAX_LENGTH = 65534
DIFF = (1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15)
SCALE = (230, 230, 230, 230, 307, 409, 512, 614)


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def encode(pcm):
    """Same as M_EncodeADPCM in audio.c; returns the packed bytes."""
    out = bytearray(((len(pcm) + 1) // 2 + 31) & ~31)
    predictor, step = 0, 127
    for i, s in enumerate(pcm):
        delta = s - predictor
        nib = min(abs(delta) * 4 // step, 7)
        if delta < 0:
            nib += 8
        p = predictor + tdiv(step * DIFF[nib], 8)
        predictor = max(-32768, min(32767, p))
        step = max(127, min(24576, (step * SCALE[nib & 7]) >> 8))
        out[i >> 1] |= nib << 4 if i & 1 else nib
    return bytes(out)


def to_mono16(fmt, ch, bits, body):
    if fmt != 1 or bits not in (8, 16) or ch < 1:
        return None
    if bits == 8:
        v = [(b - 128) << 8 for b in body]
    else:
        v = list(struct.unpack('<%dh' % (len(body) // 2), body[:len(body) // 2 * 2]))
    return [sum(v[i:i + ch]) // ch for i in range(0, len(v) - ch + 1, ch)]


def main(src, dst):
    d = open(src, 'rb').read()
    p = d.find(b'RIFF')
    out = bytearray()
    offs = []
    done = kept = 0
    while p >= 0 and p + 44 <= len(d) and d[p:p + 4] == b'RIFF':
        size = struct.unpack('<I', d[p + 40:p + 44])[0]
        end = p + 44 + ((size + 1) & ~1)
        fmt, ch, rate, _, _, bits = struct.unpack('<HHIIHH', d[p + 20:p + 36])
        pcm = (to_mono16(fmt, ch, bits, d[p + 44:p + 44 + size])
               if d[p + 8:p + 12] == b'WAVE' and d[p + 36:p + 40] == b'data'
               else None)
        offs.append(len(out))
        if not pcm:
            out += d[p:end]
            kept += 1
        else:
            while len(pcm) > MAX_LENGTH:
                n = len(pcm) // 2
                pcm = [tdiv(pcm[2 * i] + pcm[2 * i + 1], 2) for i in range(n)]
                rate //= 2
            adpcm = encode(pcm)
            hdr = b'RIFF' + struct.pack('<I', 36 + len(adpcm)) + b'WAVE'
            hdr += b'fmt ' + struct.pack('<IHHIIHH', 16, 0x20, 1, rate, len(pcm), 32, 4)
            hdr += b'data' + struct.pack('<I', len(adpcm))
            out += hdr + adpcm
            done += 1
        p = end
    open(dst, 'wb').write(out)
    open(dst + '.idx', 'wb').write(struct.pack('<%dI' % len(offs), *offs))
    print('main.sfx: %d samples to ADPCM, %d kept, %d -> %d bytes'
          % (done, kept, len(d), len(out)))


main(sys.argv[1], sys.argv[2])
