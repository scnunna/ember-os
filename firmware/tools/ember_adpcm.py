"""Reference IMA-ADPCM block codec for Ember Link (see docs/EMBER_LINK.md) + test-vector generator.

Block = 133 bytes: seq u8 | predictor int16 LE | index u8 | 0 | 128 bytes (256 x 4-bit, low nibble first).
The encoder starts each block from predictor = first sample of the block and the index carried over from the
previous block; all 256 samples (including the first, whose nibble is computed normally) are encoded.

  python tools/ember_adpcm.py vectors   -> writes docs/link_vectors.json
"""
import json, math, os, sys

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
N = 256


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def step_decode(nib, pred, index):
    step = STEP[index]
    diff = step >> 3
    if nib & 1: diff += step >> 2
    if nib & 2: diff += step >> 1
    if nib & 4: diff += step
    pred = pred - diff if nib & 8 else pred + diff
    return clamp(pred, -32768, 32767), clamp(index + IDX[nib], 0, 88)


def encode_block(samples, index, seq):
    assert len(samples) == N
    pred = samples[0]
    out = bytearray([seq & 0xFF, pred & 0xFF, (pred >> 8) & 0xFF, index, 0])
    nibs = []
    for s in samples:
        step = STEP[index]
        diff = s - pred
        nib = 0
        if diff < 0:
            nib = 8; diff = -diff
        if diff >= step: nib |= 4; diff -= step
        if diff >= step >> 1: nib |= 2; diff -= step >> 1
        if diff >= step >> 2: nib |= 1
        pred, index = step_decode(nib, pred, index)
        nibs.append(nib)
    for i in range(0, N, 2):
        out.append(nibs[i] | (nibs[i + 1] << 4))
    return bytes(out), index


def decode_block(b):
    assert len(b) == 5 + N // 2
    pred = int.from_bytes(b[1:3], 'little', signed=True)
    index = b[3]
    out = []
    for byte in b[5:]:
        for nib in (byte & 0x0F, byte >> 4):
            pred, index = step_decode(nib, pred, index)
            out.append(pred)
    return out


def test_signal():
    """Deterministic 2-block test input: 440 Hz + 1250 Hz sine at 16 kHz, amplitude ~12000."""
    return [int(round(8000 * math.sin(2 * math.pi * 440 * i / 16000) + 4000 * math.sin(2 * math.pi * 1250 * i / 16000))) for i in range(2 * N)]


def frame(msg_bytes, mtu=23):
    """Split one JSON message into GATT packets with the FIRST/LAST flag byte."""
    room = mtu - 3 - 1
    parts = [msg_bytes[i:i + room] for i in range(0, len(msg_bytes), room)] or [b'']
    pk = []
    for i, p in enumerate(parts):
        f = (1 if i == 0 else 0) | (2 if i == len(parts) - 1 else 0)
        pk.append(bytes([f]) + p)
    return pk


def vectors():
    sig = test_signal()
    b1, idx = encode_block(sig[:N], 0, 0)
    b2, idx = encode_block(sig[N:], idx, 1)
    dec = decode_block(b1) + decode_block(b2)
    err = max(abs(a - b) for a, b in zip(sig, dec))
    msg = json.dumps({"t": "note", "id": "M_20261114_093000.wav", "title": "Standup", "summary": "Shipped Ember OS.",
                      "actions": ["Buy microSD card", "Test battery"]}, separators=(',', ':')).encode()
    return {
        "adpcm": {"input": sig, "blocks_hex": [b1.hex(), b2.hex()], "decoded": dec, "max_abs_error": err},
        "framing": {"mtu": 23, "message": msg.decode(), "packets_hex": [p.hex() for p in frame(msg)]},
    }


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'vectors':
        v = vectors()
        p = os.path.join(os.path.dirname(__file__), '..', '..', 'docs', 'link_vectors.json')
        json.dump(v, open(p, 'w'), indent=1)
        print('wrote', os.path.normpath(p), 'adpcm max error', v['adpcm']['max_abs_error'], 'packets', len(v['framing']['packets_hex']))
    else:
        print(__doc__)
