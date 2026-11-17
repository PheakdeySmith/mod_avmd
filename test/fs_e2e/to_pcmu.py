#!/usr/bin/env python3
"""Convert 8 kHz 16-bit mono corpus WAVs to raw G.711 mu-law (.PCMU) for sip_callee.py.

  to_pcmu.py <corpus_dir> <out_dir>
"""
import glob
import os
import sys
import wave

import numpy as np


def lin2ulaw(pcm):
    """ITU-T G.711 mu-law encoder (no audioop: removed in Python 3.13)."""
    BIAS, CLIP = 0x84, 32635
    x = pcm.astype(np.int32)
    sign = (x < 0).astype(np.int32) << 7
    mag = np.minimum(np.abs(x), CLIP) + BIAS
    exp = np.floor(np.log2(mag)).astype(np.int32) - 7
    exp = np.clip(exp, 0, 7)
    mant = (mag >> (exp + 3)) & 0x0F
    return (~(sign | (exp << 4) | mant) & 0xFF).astype(np.uint8)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    n = 0
    for p in sorted(glob.glob(os.path.join(src, "*.wav"))):
        with wave.open(p) as w:
            if w.getframerate() != 8000 or w.getsampwidth() != 2 or w.getnchannels() != 1:
                print("skip (need 8 kHz 16-bit mono):", p)
                continue
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        name = os.path.splitext(os.path.basename(p))[0] + ".PCMU"
        with open(os.path.join(dst, name), "wb") as f:
            f.write(lin2ulaw(pcm).tobytes())
        n += 1
    print("wrote %d .PCMU files to %s" % (n, dst))


if __name__ == "__main__":
    main()
