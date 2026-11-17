#!/usr/bin/env python3
"""
Synthetic AMD test corpus.

Not a substitute for real recordings: it exercises the engine's timing logic,
tone detection and message-end tracking across the strata that break AMD in
production (short hello, double hello, business greeting, silent pickup,
mid-greeting pause, no-beep voicemail, music-on-greeting, fax, SIT, noise).

Speech is a source-filter model: jittered glottal pulse train through three
formant resonators per syllable plus fricative noise, with syllable envelopes.
That is close enough to real speech that a pure-tone detector must reject it
and an energy VAD must accept it.

Writes 8 kHz mu-law WAVs (as off a PSTN trunk) and labels.csv with
file,stratum,truth,greeting_end_ms,beep_end_ms
"""
import argparse
import csv
import os
import wave

import numpy as np
from scipy.signal import lfilter

SR = 8000
rng = np.random.default_rng(7)

VOWELS = [(730, 1090, 2440), (270, 2290, 3010), (300, 870, 2240), (530, 1840, 2480),
          (660, 1720, 2410), (490, 1350, 1690), (640, 1190, 2390)]


def resonator(x, f, bw):
    r = np.exp(-np.pi * bw / SR)
    th = 2 * np.pi * f / SR
    return lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], x)


def syllable(dur, f0):
    n = int(dur * SR)
    t = np.arange(n)
    f0t = f0 * (1 + 0.08 * np.sin(2 * np.pi * rng.uniform(2, 5) * t / SR)) * (1 + 0.01 * rng.standard_normal(n))
    phase = np.cumsum(f0t / SR)
    src = (np.diff(np.floor(phase), prepend=0) > 0).astype(float)
    src = lfilter([1], [1, -0.95], src)  # glottal tilt
    f1, f2, f3 = VOWELS[rng.integers(len(VOWELS))]
    v = resonator(src, f1, 90) + 0.6 * resonator(src, f2, 110) + 0.3 * resonator(src, f3, 160)
    if rng.random() < 0.5:  # fricative onset
        k = int(min(0.07, dur / 3) * SR)
        v[:k] = v[:k] * 0.3 + resonator(rng.standard_normal(k), 2500, 1200) * 0.4
    env = np.sin(np.pi * np.linspace(0, 1, n)) ** 0.6
    v = v * env
    return v / (np.max(np.abs(v)) + 1e-9)


def speech(dur, f0=None, level_db=-20, pause_p=0.15):
    """Continuous speech ~dur seconds, syllables 120-280 ms, small inter-word gaps."""
    f0 = f0 or rng.uniform(95, 230)
    out = []
    total = 0.0
    while total < dur:
        d = rng.uniform(0.12, 0.28)
        out.append(syllable(d, f0 * rng.uniform(0.9, 1.1)))
        total += d
        if rng.random() < pause_p:  # inter-word gap (below segment gap)
            g = rng.uniform(0.04, 0.18)
            out.append(np.zeros(int(g * SR)))
            total += g
    x = np.concatenate(out)[: int(dur * SR)]
    rms = np.sqrt(np.mean(x ** 2)) + 1e-9
    return x / rms * 10 ** (level_db / 20)


def tone(freq, dur, level_db=-14):
    t = np.arange(int(dur * SR)) / SR
    x = np.sin(2 * np.pi * freq * t)
    ramp = min(len(x) // 10, 40)
    if ramp:
        x[:ramp] *= np.linspace(0, 1, ramp)
        x[-ramp:] *= np.linspace(1, 0, ramp)
    return x * 10 ** (level_db / 20) * np.sqrt(2)


def music(dur, level_db=-22):
    """Piano-ish notes with harmonics: should NOT be taken as beeps."""
    out = []
    total = 0.0
    while total < dur:
        d = rng.uniform(0.18, 0.45)
        f = 220 * 2 ** (rng.integers(0, 15) / 12)
        t = np.arange(int(d * SR)) / SR
        x = sum((0.7 ** h) * np.sin(2 * np.pi * f * (h + 1) * t) for h in range(5))
        x *= np.exp(-t * 4)
        out.append(x)
        total += d
    x = np.concatenate(out)[: int(dur * SR)]
    return x / (np.sqrt(np.mean(x ** 2)) + 1e-9) * 10 ** (level_db / 20)


def sil(d):
    return np.zeros(int(d * SR))


def cat(*parts):
    return np.concatenate(parts)


def finalize(x, snr_db=None, total=12.0):
    n = int(total * SR)
    if len(x) < n:
        x = np.concatenate([x, np.zeros(n - len(x))])
    x = x[:n]
    noise_db = -62 if snr_db is None else -20 - snr_db
    x = x + rng.standard_normal(n) * 10 ** (noise_db / 20)
    return np.clip(x, -0.99, 0.99)


def lin2ulaw(x):
    mu = 255
    y = np.sign(x) * np.log1p(mu * np.abs(x)) / np.log1p(mu)
    q = np.round((y + 1) / 2 * 255).astype(np.int16)
    return q


def write_ulaw(path, x):
    # encode to G.711 mu-law via audioop-free path: quantize companded then expand back to PCM16
    # (keeps file PCM16 for portability but with true mu-law quantization noise)
    mu = 255
    q = lin2ulaw(x)
    y = q / 255 * 2 - 1
    xr = np.sign(y) * ((1 + mu) ** np.abs(y) - 1) / mu
    pcm = np.clip(xr * 32767, -32768, 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def scenarios():
    """Yield (stratum, truth, audio, greeting_end_s, beep_end_s)."""
    u = rng.uniform
    for _ in range(N):
        # ----- humans -----
        a = u(0.3, 1.2); h = u(0.35, 0.9)
        yield 'human_hello', 'human', cat(sil(a), speech(h), sil(2.5), speech(0.5)), a + h, None
        a = u(0.3, 1.0); h1 = u(0.35, 0.7); g = u(1.5, 2.5); h2 = u(0.35, 0.7)
        yield 'human_double_hello', 'human', cat(sil(a), speech(h1), sil(g), speech(h2)), a + h1, None
        a = u(0.3, 0.9); h = u(1.9, 2.6)
        yield 'human_business', 'human', cat(sil(a), speech(h, pause_p=0.25), sil(4)), a + h, None
        a = u(3.6, 4.5); h = u(0.4, 0.8)
        yield 'human_silent_pickup', 'human', cat(sil(a), speech(h), sil(3)), a + h, None
        a = u(0.3, 1.0); h = u(0.4, 0.9)
        yield 'human_noisy_10db', 'human', cat(sil(a), speech(h), sil(4)), a + h, None
        # ----- machines -----
        a = u(0.2, 0.8); g = u(4.0, 8.0); bf = rng.choice([1000, 850, 1400, 440, 950]); bd = u(0.25, 0.6)
        p = u(0.2, 0.6)
        yield 'vm_carrier_beep', 'machine', cat(sil(a), speech(g, pause_p=0.2), sil(p), tone(bf, bd), sil(3)), a + g, a + g + p + bd
        a = u(0.2, 0.8); g = u(1.6, 2.4); bd = u(0.3, 0.5)
        yield 'vm_short_custom', 'machine', cat(sil(a), speech(g), sil(0.4), tone(1000, bd), sil(3)), a + g, a + g + 0.4 + bd
        a = u(0.2, 0.8); g1 = u(1.4, 2.2); p = u(1.0, 1.4); g2 = u(2.0, 3.0)
        yield 'vm_mid_pause', 'machine', cat(sil(a), speech(g1), sil(p), speech(g2), sil(0.5), tone(1000, 0.4), sil(3)), a + g1 + p + g2, a + g1 + p + g2 + 0.9
        a = u(0.2, 0.8); g = u(4.0, 7.0)
        yield 'vm_no_beep', 'machine', cat(sil(a), speech(g, pause_p=0.2), sil(5)), a + g, None
        a = u(0.2, 0.6); m = u(2.0, 3.0); g = u(3.0, 5.0)
        yield 'vm_music_greeting', 'machine', cat(sil(a), music(m), speech(g), sil(0.3), tone(1000, 0.5), sil(3)), a + m + g, a + m + g + 0.8
        a = u(0.2, 0.8); g = u(4.0, 7.0)
        yield 'vm_noisy_10db', 'machine', cat(sil(a), speech(g, pause_p=0.2), sil(0.4), tone(1000, 0.4), sil(3)), a + g, a + g + 0.8
        a = u(0.3, 0.8)
        yield 'vm_beep_only', 'machine', cat(sil(a), tone(1000, 0.5), sil(5)), a, a + 0.5
        # ----- others -----
        yield 'fax_ced', 'fax', cat(sil(u(0.2, 0.8)), tone(2100, 3.3, -12), sil(2)), None, None
        a = u(0.1, 0.5)
        yield 'sit_tone', 'unavailable', cat(sil(a), tone(913.8, 0.274), tone(1370.6, 0.274), tone(1776.7, 0.380), sil(0.3), speech(3)), None, None


N = 40

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('-n', type=int, default=40, help='samples per stratum')
    ap.add_argument('--seed', type=int, default=7, help='RNG seed (use different seeds for train and held-out corpora)')
    args = ap.parse_args()
    N = args.n
    rng = np.random.default_rng(args.seed)
    os.makedirs(args.outdir, exist_ok=True)
    rows = []
    for i, (stratum, truth, x, gend, bend) in enumerate(scenarios()):
        snr = 10 if 'noisy' in stratum else None
        # beep end: find the tone position for message-end timing checks
        name = f'{stratum}_{i:04d}.wav'
        write_ulaw(os.path.join(args.outdir, name), finalize(x, snr))
        rows.append((name, stratum, truth, '' if gend is None else int(gend * 1000), '' if bend is None else int(bend * 1000)))
    with open(os.path.join(args.outdir, 'labels.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['file', 'stratum', 'truth', 'greeting_end_ms', 'beep_end_ms'])
        w.writerows(rows)
    print(f'wrote {len(rows)} files to {args.outdir}')
