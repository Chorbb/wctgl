#!/usr/bin/env python3
"""labyrinth.py — расшифровка записей комнат лабиринта (банк 5, $8A6E, 24×24 подряд).

    python3 tools/labyrinth.py rom.nes            сводка + карта дверей
    python3 tools/labyrinth.py rom.nes --rooms    по строке на комнату
    python3 tools/labyrinth.py rom.nes --check    двери соседей согласованы (проверка расшифровки)

Формат записи (по коду $8071/$8121/$8155, банк 5):
  байт 0: бит 7 — особая комната (вход в коридор, магазин…: строится кодом $EE1D банка 7);
          биты 6-5 → $5D (при $20/$40 есть доп. байт → $5E); бит 4 — есть узор блоков;
          биты 3-0 — длина записи − 1. Ровно $80 — комнаты нет.
  байт 1: биты 7/6/5/4 — проходы на север/запад/восток/юг (нет бита — стена, патч $A6EB);
          бит 3 → флаг $57.
  байт 2: обычная комната — зона в битах 3-0 (шаблон $A7B0/$A7CA по $50);
          особая — номер особой комнаты в битах 6-0 (таблица $EF40 → $55).
  далее: [байт $5E] [байт для $57] [указатель на узор блоков, 2 байта, если бит 4]
  узор: байт 0 → $8042 (вид блока), байт 1 — значение клетки; поле — 18 байт битов
        (16×9 клеток, ряды 2-10 сетки $740).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import Rom

BASE = 0x8A6E
DOORS = (('N', 0x80, 0, -1, 0x10), ('W', 0x40, -1, 0, 0x20), ('E', 0x20, 1, 0, 0x40), ('S', 0x10, 0, 1, 0x80))


def read(rom):
    rooms, p = {}, BASE
    for y in range(24):
        for x in range(24):
            b0 = rom.byte(5, p)
            n = (b0 & 15) + 1
            rooms[(x, y)] = dict(addr=p, raw=[rom.byte(5, p + k) for k in range(n)])
            p += n
    for (x, y), rm in rooms.items():
        r = rm['raw']
        rm['exists'] = r[0] != 0x80
        if not rm['exists']: continue
        rm['special'] = bool(r[0] & 0x80)
        rm['doors'] = ''.join(d for d, bit, *_ in DOORS if len(r) > 1 and r[1] & bit)
        rm['zone'] = None if rm['special'] or len(r) < 3 else r[2] & 15
        rm['kind'] = r[2] & 0x7F if rm['special'] and len(r) > 2 else None
        i = 3
        k5d = r[0] & 0x60
        if k5d and k5d != 0x60: i += 1
        if len(r) > 1 and r[1] & 0x08: i += 1
        rm['pattern'] = (r[i] | r[i + 1] << 8) if r[0] & 0x10 and i + 1 < len(r) else None
    return rooms, p


def check(rooms):
    ok = bad = 0
    for (x, y), rm in rooms.items():
        if not rm['exists']: continue
        for d, bit, dx, dy, opp in DOORS:
            nb = rooms.get((x + dx, y + dy))
            if rm['raw'][1] & bit and nb and nb['exists']:
                if len(nb['raw']) > 1 and nb['raw'][1] & opp: ok += 1
                else: bad += 1; print('несогласовано: %d,%d %s' % (x, y, d))
    return ok, bad


def door_map(rooms):
    out = []
    for y in range(24):
        top, mid = '', ''
        for x in range(24):
            rm = rooms[(x, y)]
            if not rm['exists']: top += '   '; mid += '   '; continue
            c = '*' if rm['special'] else '%X' % rm['zone'] if rm['zone'] is not None else '?'
            top += ' | ' if 'N' in rm['doors'] else ' . '
            mid += ('-' if 'W' in rm['doors'] else ' ') + c + ('-' if 'E' in rm['doors'] else ' ')
        out += [top.rstrip(), mid.rstrip()]
    return '\n'.join(out)


def main():
    rom = Rom(sys.argv[1])
    rooms, end = read(rom)
    real = [r for r in rooms.values() if r['exists']]
    if '--check' in sys.argv:
        ok, bad = check(rooms)
        print('двери: согласовано %d, несогласовано %d' % (ok, bad))
        sys.exit(1 if bad else 0)
    if '--rooms' in sys.argv:
        for (x, y), rm in sorted(rooms.items(), key=lambda t: (t[0][1], t[0][0])):
            if not rm['exists']: continue
            print('%2d,%2d $%04X %-11s %-4s %s%s' % (
                x, y, rm['addr'], ' '.join('%02X' % v for v in rm['raw']), rm['doors'],
                'особая %d' % rm['kind'] if rm['special'] else 'зона %d' % rm['zone'],
                ' узор $%04X' % rm['pattern'] if rm['pattern'] else ''))
        return
    print('записи $%04X-$%04X, комнат %d (особых %d), с узором блоков %d' % (
        BASE, end - 1, len(real), sum(r['special'] for r in real), sum(bool(r['pattern']) for r in real)))
    print(door_map(rooms))


if __name__ == '__main__':
    main()
