"""dumpload.py — чтение build/dump.bin (xtest dump): массивы памяти, экранов и спрайтов по кадрам."""
import numpy as np

REC = 6 + 2048 + 2048 + 256


def load(path='build/dump.bin'):
    raw = np.fromfile(path, dtype=np.uint8)
    n = len(raw) // REC
    r = raw[:n * REC].reshape(n, REC)
    seg = r[:, 1].astype(int) | (r[:, 2].astype(int) << 8)
    frame = r[:, 3].astype(int) | (r[:, 4].astype(int) << 8)
    pad = r[:, 5]
    ram = r[:, 6:6 + 2048]
    nt = r[:, 6 + 2048:6 + 4096]
    oam = r[:, 6 + 4096:]
    return seg, frame, pad, ram, nt, oam
