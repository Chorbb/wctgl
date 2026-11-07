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
other = collections.defaultdict(set)
if os.path.exists('build/statwatch.txt'):
    for l in open('build/statwatch.txt'):
        c, _, kind, pc, bank, typ, val, src, idx, pb = l.split()
        if kind == 'hp' and pc not in ('CFB4', 'D327', '864B', '86D3', '85F0'):
            other[int(typ, 16) & 0x7F].add('$%s,%s' % (pc, src if src not in ('FFFF', '0000') else ('=%d' % int(idx) if src == 'FFFF' else 'расчёт')))
print('# Типы объектов The Guardian Legend (таблицы в ROM, банк 7)\n')
print('Индекс — номер типа (obj_type & $7F). Адрес байта: таблица + номер типа.')
print('Здоровье врагов коридоров задают ещё и таблицы банка 6 — здесь не все.\n')
ai = {t: (rom.byte(6, 0x80DE + t) | rom.byte(6, 0x815D + t) << 8) + 1 for t in range(127)}
share = collections.Counter(ai.values())
print('Поведение — процедура из таблицы банка 6 ($80DE/$815D, вызов $8218). «свободен» — процедура')
print('$C048 сразу освобождает ячейку: тип в игре не используется, его можно занять новым врагом.\n')
print('| тип | поведение | здоровье $D0AB | урон $D12A | параметр $D02C | флаги $D1A9 | встречен в зонах | здоровье задаёт также |')
print('|---|---|---|---|---|---|---|---|')
for t in range(1, 128):
    zones = ', '.join(str(z) for z, _ in sorted(seen[t].items())) if seen[t] else ''
    beh = '—' if t >= 127 else ('свободен' if ai[t] == 0xC048 else '$%04X%s' % (ai[t], ' (общая с %d типами)' % (share[ai[t]] - 1) if share[ai[t]] > 1 else ''))
    print('| %d ($%02X) | %s | %d | %d | %d | $%02X | %s | %s |' % (t, t, beh, tab['hp'][t], tab['dmg'][t], tab['param'][t], tab['flags'][t], zones,
                                                        ' '.join(sorted(other[t]))))

names = {3: 'волна', 5: 'граната', 6: 'огненный шар', 8: 'отражатель'}
print('\n# Спецоружие (банк 6): урон $86F5 и здоровье снаряда $8716\n')
print('Индекс = 11 × уровень + номер оружия (уровень 1..3). Урон 0 — берётся урон основной пушки.\n')
print('| номер | оружие | урон ур.1 | ур.2 | ур.3 | здоровье снаряда ур.1 | ур.2 | ур.3 |')
print('|---|---|---|---|---|---|---|---|')
for w in range(11):
    d = [rom.byte(6, 0x86F5 + 11 * L + w) for L in (1, 2, 3)]
    h = [rom.byte(6, 0x8716 + 11 * L + w) for L in (1, 2, 3)]
    print('| %d | %s | %d | %d | %d | %d | %d | %d |' % (w, names.get(w, '?'), d[0], d[1], d[2], h[0], h[1], h[2]))
