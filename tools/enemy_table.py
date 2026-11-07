#!/usr/bin/env python3
"""enemy_table.py — таблица характеристик типов объектов из ROM (банк 7, $D02C/$D0AB/$D12A/$D1A9).

    python3 tools/enemy_table.py rom.nes > build/enemy_types.md

Пишет в build/ (данные из ROM в репозиторий не кладём). Какие типы встречаются в записи
и в каких зонах — из build/dump.bin, если он есть.
"""
import sys, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import Rom

rom = Rom(sys.argv[1])
tab = {k: [rom.byte(7, a + t) for t in range(128)] for k, a in
       (('param', 0xD02C), ('hp', 0xD0AB), ('dmg', 0xD12A), ('flags', 0xD1A9))}
seen = collections.defaultdict(collections.Counter)
if os.path.exists('build/dump.bin'):
    from dumpload import load
    seg, frame, pad, ram, nt, oam = load('build/dump.bin')
    for i in range(0, len(ram), 3):
        for s in range(1, 24):
            t = int(ram[i][0x500 + s]) & 0x7F
            if t:
                seen[t][int(ram[i][0x50])] += 1
print('# Типы объектов The Guardian Legend (таблицы в ROM, банк 7)\n')
print('Индекс — номер типа (obj_type & $7F). Адрес байта: таблица + номер типа.')
print('Здоровье врагов коридоров задают ещё и таблицы банка 6 — здесь не все.\n')
print('| тип | здоровье $D0AB | урон $D12A | параметр $D02C | флаги $D1A9 | встречен в зонах |')
print('|---|---|---|---|---|---|')
for t in range(1, 128):
    zones = ', '.join(str(z) for z, _ in sorted(seen[t].items())) if seen[t] else ''
    print('| %d ($%02X) | %d | %d | %d | $%02X | %s |' % (t, t, tab['hp'][t], tab['dmg'][t], tab['param'][t], tab['flags'][t], zones))
