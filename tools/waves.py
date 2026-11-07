#!/usr/bin/env python3
"""waves.py — расшифровка сценариев волн врагов (интерпретатор $F78F, банк 7).

    python3 tools/waves.py rom.nes build/scripts.txt > build/waves.md

Сценарий читается по указателю $66/$67 в банке $7F. Формат (по коду $F7BD-$F85C):
  0  N          — пауза: N тиков ($68)
  1  lo hi      — перейти на адрес hi:lo
  2             — конец сценария
  T (3..$7F)    — враг типа T (появляется в свободной ячейке; позиция — $FA5B/$EA8C)
  $80|T  a  b   — генератор (объект типа $29) для врага T с параметрами a→$560, b→$7A0
  после врага/генератора байт D: D ≥ $80 — сразу следующий враг (D — его тип),
                                 D = 0 — продолжить без паузы, иначе пауза D тиков.
Начала сценариев — из записи (xtest statwatch XT_SCRIPT=1); данные из ROM — только в build/.
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import Rom

rom = Rom(sys.argv[1])
hp = [rom.byte(7, 0xD0AB + t) for t in range(128)]


def rd(bank, a):
    return rom.byte(bank, a & 0xFFFF)


def decode(bank, ptr, limit=200):
    out, seen, y = [], set(), 0
    while len(out) < limit:
        if (ptr + y) in seen and y == 0:
            out.append(('цикл', ptr)); break
        if y == 0: seen.add(ptr)
        b = rd(bank, ptr + y); y += 1
        if b == 0:
            out.append(('пауза', rd(bank, ptr + y))); y += 1; ptr += y; y = 0; continue
        if b == 1:
            tgt = rd(bank, ptr + y) | rd(bank, ptr + y + 1) << 8
            out.append(('переход', tgt))
            if tgt in seen: out.append(('цикл', tgt)); break
            ptr, y = tgt, 0; continue
        if b == 2:
            out.append(('конец', None)); break
        t = b
        while True:
            if t & 0x80:
                a1, a2 = rd(bank, ptr + y), rd(bank, ptr + y + 1); y += 2
                out.append(('генератор', (t & 0x7F, a1, a2)))
            else:
                out.append(('враг', t))
            d = rd(bank, ptr + y); y += 1
            if d & 0x80:
                t = d; continue
            if d == 0:
                y += 1          # так в коде: INY перед следующим чтением
                break
            out.append(('пауза', d)); ptr += y; y = 0
            break
    return out


if __name__ != '__main__':
    raise SystemExit  # модуль используется только для decode()

starts = collections.OrderedDict()
for line in open(sys.argv[2]):
    p = line.split()
    if p[0] == 'P':
        starts.setdefault((int(p[2]), int(p[1], 16)), (int(p[3]), int(p[4]), int(p[5])))
print('# Сценарии волн врагов\n')
print('Из записи прохождения: каждый сценарий — откуда он начался (банк, адрес) и где был игрок')
print('(зона, X, Y). Тип врага — номер из таблицы enemy_types.md, рядом — его здоровье из $D0AB.\n')
for (bank, ptr), (area, x, y) in sorted(starts.items()):
    print('## Банк %d, $%04X — зона %d, X %d, Y %d\n' % (bank, ptr, area, x, y))
    for kind, v in decode(bank, ptr):
        if kind == 'враг': print('- враг тип %d (здоровье %d)' % (v, hp[v]))
        elif kind == 'генератор': print('- генератор врагов тип %d (параметры %d, %d)' % (v[0], v[1], v[2]))
        elif kind == 'пауза': print('- пауза %d' % v)
        elif kind in ('переход', 'цикл'): print('- %s на $%04X' % (kind, v))
        else: print('- конец')
    print()
