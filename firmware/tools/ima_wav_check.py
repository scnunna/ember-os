"""Checks the watch's compressed meeting-recording format (IMA-ADPCM WAV, format 0x11) against ffmpeg.
A line-by-line Python port of ima_wav_encode_block (firmware/main/adpcm.c) + ima_header (rec.c) writes a test file;
ffmpeg (an independent decoder) must read it, and its samples must match this port's own decoder exactly.
Also writes companion/app/src/test/resources/ima_vector.{wav,pcm} so the phone's decoder is tested on the same bytes.
Usage: python tools/ima_wav_check.py"""
import math, os, struct, subprocess, sys

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
SPB, BA, RATE = 505, 256, 16000
clamp = lambda v, lo, hi: lo if v < lo else hi if v > hi else v


def encode_block(s, idx):
    pred = s[0]
    out = bytearray(BA)
    struct.pack_into('<hBB', out, 0, pred, idx, 0)
    for i in range(1, SPB):
        step, diff, nib = STEP[idx], s[i] - pred, 0
        if diff < 0: nib, diff = 8, -diff
        if diff >= step: nib |= 4; diff -= step
        if diff >= step >> 1: nib |= 2; diff -= step >> 1
        if diff >= step >> 2: nib |= 1
        dq = step >> 3
        if nib & 1: dq += step >> 2
        if nib & 2: dq += step >> 1
        if nib & 4: dq += step
        pred = clamp(pred - dq if nib & 8 else pred + dq, -32768, 32767)
        idx = clamp(idx + IDX[nib], 0, 88)
        j = i - 1
        if j & 1: out[4 + (j >> 1)] |= nib << 4
        else: out[4 + (j >> 1)] = nib
    return bytes(out), idx


def decode_block(b):
    pred, idx = struct.unpack_from('<hB', b, 0)
    res = [pred]
    for j in range(SPB - 1):
        nib = (b[4 + (j >> 1)] >> (4 if j & 1 else 0)) & 15
        step = STEP[idx]; dq = step >> 3
        if nib & 1: dq += step >> 2
        if nib & 2: dq += step >> 1
        if nib & 4: dq += step
        pred = clamp(pred - dq if nib & 8 else pred + dq, -32768, 32767)
        idx = clamp(idx + IDX[nib], 0, 88)
        res.append(pred)
    return res


def header(samples, data):
    return (b'RIFF' + struct.pack('<I', 52 + data) + b'WAVEfmt ' + struct.pack('<IHHIIHHHH', 20, 0x11, 1, RATE, RATE * BA // SPB, BA, 4, 2, SPB)
            + b'fact' + struct.pack('<II', 4, samples) + b'data' + struct.pack('<I', data))


# 1.3 s of speech-like test audio: two tones + a burst + silence, partial last block like the watch's final block
n = int(RATE * 1.3)
pcm = [int(9000 * math.sin(2 * math.pi * 220 * t / RATE) + 4000 * math.sin(2 * math.pi * 1750 * t / RATE) * (1 if (t // 2000) % 2 else 0)) for t in range(n)]
blocks, idx, dec = [], 0, []
for k in range(0, n, SPB):
    blk = pcm[k:k + SPB]
    have = len(blk)
    blk = blk + [blk[-1]] * (SPB - have)
    b, idx = encode_block(blk, idx)
    blocks.append(b)
    dec += decode_block(b)
dec = dec[:n]
data = b''.join(blocks)
wav = header(n, len(data)) + data
here = os.path.dirname(os.path.abspath(__file__))
res_dir = os.path.join(here, '..', '..', 'companion', 'app', 'src', 'test', 'resources')
os.makedirs(res_dir, exist_ok=True)
open(os.path.join(res_dir, 'ima_vector.wav'), 'wb').write(wav)
open(os.path.join(res_dir, 'ima_vector.pcm'), 'wb').write(struct.pack(f'<{n}h', *dec))

ok = True
p = subprocess.run(['ffmpeg', '-v', 'error', '-i', '-', '-f', 's16le', '-ac', '1', '-'], input=wav, capture_output=True)
ff = list(struct.unpack(f'<{len(p.stdout) // 2}h', p.stdout)) if p.returncode == 0 else []
print(('PASS' if p.returncode == 0 else 'FAIL') + ' ffmpeg reads the file', p.stderr.decode()[:200])
same = ff[:n] == dec
print(('PASS' if same else 'FAIL') + f' ffmpeg samples == our decoder ({len(ff)} vs {n} samples)')
ok &= p.returncode == 0 and same
err = math.sqrt(sum((a - b) ** 2 for a, b in zip(pcm, dec)) / n)
sig = math.sqrt(sum(a * a for a in pcm) / n)
snr = 20 * math.log10(sig / max(err, 1e-9))
print(('PASS' if snr > 20 else 'FAIL') + f' quality: SNR {snr:.1f} dB (speech is fine above ~20 dB)')
ok &= snr > 20
print(f'size: {len(wav)} bytes vs {44 + 2 * n} as 16-bit PCM = {(44 + 2 * n) / len(wav):.2f}x smaller')
# the same integer signal the watch's "imatest" shell command encodes: both must print the same checksum
sig = [((t * 37) % 2001 - 1000) * 13 for t in range(2 * SPB)]
idx2, chk = 0, 0
for k in range(2):
    b, idx2 = encode_block(sig[k * SPB:(k + 1) * SPB], idx2)
    for x in b: chk = (chk * 31 + x) & 0xFFFFFFFF
print(f'expected watch "imatest": sum={chk:08x} idx={idx2}')
sys.exit(0 if ok else 1)
