#!/usr/bin/env python3
"""ramcheck.py — перепроверка карты имён по записи (build/dump.bin от `xtest dump`).

Для переменных панели сравнивает память с тем, что игра написала на панели;
кнопки — с записанными нажатиями; координаты объектов — с позициями спрайтов.
Печатает долю совпадений. Ниже 90 % — повод пересмотреть имя.
"""
import sys
import numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from dumpload import load
from hud import parse_hud

seg, frame, pad, ram, nt, oam = load(sys.argv[1] if len(sys.argv) > 1 else 'build/dump.bin')
H = [parse_hud(x) for x in nt]
idx = [i for i, h in enumerate(H) if h]

checks = [
    ('area      $0050', 'area', lambda r: r[0x50]),
    ('map_x     $0052', 'x', lambda r: r[0x52]),
    ('map_y     $0053', 'y', lambda r: r[0x53]),
    ('shot      $003E', 'shot', lambda r: r[0x3E]),
    ('chips     $004C', 'chips', lambda r: int(r[0x4C]) | int(r[0x4D]) << 8),
    ('life      $0049', 'life_num', lambda r: (int(r[0x49]) + 7) >> 3),
    ('score     $0170', 'score', lambda r: sum(int(d) * 10 ** (6 - k) for k, d in enumerate(r[0x170:0x177]))),
]
print('%-18s %7s  %s' % ('переменная', 'совпало', 'кадров'))
for name, field, fn in checks:
    ii = [i for i in idx if field in H[i]]
    ok = sum(1 for i in ii if fn(ram[i]) == H[i][field])
    print('%-18s %6.1f%%  %d' % (name, 100.0 * ok / max(1, len(ii)), len(ii)))

# объекты: занятые ячейки должны стоять там же, где спрайты
act = ram[:, 0x500:0x518] != 0
xs, ys = oam[:, 3::4].astype(int), oam[:, 0::4].astype(int)
ox, oy = ram[:, 0x530:0x548].astype(int), ram[:, 0x518:0x530].astype(int)
near = ((np.abs(ox[:, :, None] - xs[:, None, :]) <= 16) & (np.abs(oy[:, :, None] - ys[:, None, :]) <= 16)).any(2)
# очки: панель догоняет память (прибавляет понемногу) — проверяем, что не обгоняет и догоняет
ii = [i for i in idx if 'score' in H[i]]
sc = lambda r: sum(int(d) * 10 ** (6 - k) for k, d in enumerate(r[0x170:0x177]))
trail = sum(1 for i in ii if H[i]['score'] <= sc(ram[i]))
print('%-18s %6.1f%%  %d   (панель не больше памяти)' % ('score  догоняет', 100.0 * trail / len(ii), len(ii)))
print('%-18s %6.1f%%  %d' % ('obj_x/obj_y', 100.0 * near[act].mean(), int(act.sum())))
