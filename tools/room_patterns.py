#!/usr/bin/env python3
"""room_patterns.py — узоры блоков в комнатах лабиринта (код $8243-$82DC, банк 5).

    python3 tools/room_patterns.py rom.nes --check build/roomgen.txt
    python3 tools/room_patterns.py rom.nes --sheet build/rooms.txt build/roomgen.txt build/patterns.png

Узор (на него указывает запись комнаты, см. labyrinth.py):
  байт 0: биты 4-0 — номер битовой карты (карта = 18 байт с $91EA + 18·номер: 16×9 клеток,
          ряды 2-10 сетки, старший бит — левая клетка); биты 7-5 — число добавок;
  байт 1: значение клетки для битовой карты;
  добавки по 3 байта: значение клетки, номер клетки (0-191), маска: биты 5-0 — до 6 клеток
          подряд (младший бит первый), биты 7-6 — шаг: 0 вверх-вправо, 1 вправо, 2 вниз,
          3 вниз-вправо.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import Rom
import labyrinth

STEP = (-15, 1, 16, 17)


def decode(rom, addr):
    b0, val = rom.byte(5, addr), rom.byte(5, addr + 1)
    bmp = 0x91EA + 18 * (b0 & 0x1F)
    cells = {}
    for i in range(18):
        byte = rom.byte(5, bmp + i)
        for k in range(8):
            if byte & (0x80 >> k): cells[0x20 + i * 8 + k] = val
    extras = []
    p = addr + 2
    for _ in range((b0 >> 5) & 7):
        v, x, m = rom.byte(5, p), rom.byte(5, p + 1), rom.byte(5, p + 2)
        p += 3
        extras.append((v, x, m))
        for k in range(6):
            if m & (1 << k): cells[x & 0xFF] = v
            x = (x + STEP[m >> 6]) & 0xFF
    return dict(addr=addr, bitmap=b0 & 0x1F, value=val, extras=extras, cells=cells, size=p - addr)


def used(rom):
    rooms, _ = labyrinth.read(rom)
    by = {}
    for k, rm in rooms.items():
        if rm.get('exists') and rm.get('pattern'): by.setdefault(rm['pattern'], []).append(k)
    return rooms, by


def gen_cells(path):
    out = {}
    for line in open(path):
        if line.startswith('M '):
            q = line.split(); out[(int(q[1]), int(q[2]))] = bytes.fromhex(q[7])
    return out


def check(rom, gen_path):
    rooms, by = used(rom)
    g = gen_cells(gen_path)
    ok = bad = 0
    for addr, ks in by.items():
        pat = decode(rom, addr)
        for k in ks:
            if rooms[k]['special'] or k not in g: continue
            for c, v in pat['cells'].items():
                if g[k][c] == v: ok += 1
                else: bad += 1
    return ok, bad


def sheet(rom, rooms_path, gen_path, out):
    from PIL import Image, ImageDraw, ImageFont
    try: font = ImageFont.truetype('DejaVuSans.ttf', 14)
    except OSError: font = None
    import labyrinth_map as LM
    rooms, by = used(rom)
    chr_by_hash, look = LM.load_all(rom, rooms_path, gen_path)
    pats = sorted(by)
    cols = 6
    W, H = 256, 192 + 40
    img = Image.new('RGB', (cols * (W + 8), ((len(pats) + cols - 1) // cols) * (H + 8)), (24, 24, 24))
    dr = ImageDraw.Draw(img)
    for n, addr in enumerate(pats):
        ks = sorted(by[addr], key=lambda t: (t[1], t[0]))
        pat = decode(rom, addr)
        k = next((k for k in ks if not rooms[k]['special']), ks[0])
        ox, oy = (n % cols) * (W + 8) + 4, (n // cols) * (H + 8) + 4
        rm = look.get(k)
        if rm: img.paste(LM.room_image(rom, chr_by_hash, rm), (ox, oy))
        # клетки узора — рамкой поверх комнаты
        for c in pat['cells']:
            x, y = ox + (c % 16) * 16, oy + (c // 16) * 16
            dr.rectangle((x, y, x + 15, y + 15), outline=(255, 220, 0))
        rl = ' '.join('%d,%d' % t for t in ks[:6]) + (' …' if len(ks) > 6 else '')
        dr.text((ox, oy + 194), 'узор %d  $%04X  комнат: %d' % (n + 1, addr, len(ks)), fill=(255, 255, 255), font=font)
        dr.text((ox, oy + 210), rl, fill=(180, 180, 180), font=font)
    img.save(out)
    return len(pats)


def main():
    rom = Rom(sys.argv[1])
    if '--check' in sys.argv:
        ok, bad = check(rom, sys.argv[sys.argv.index('--check') + 1])
        print('клетки узоров: совпало %d, не совпало %d' % (ok, bad))
        sys.exit(1 if bad else 0)
    if '--sheet' in sys.argv:
        i = sys.argv.index('--sheet')
        n = sheet(rom, sys.argv[i + 1], sys.argv[i + 2], sys.argv[i + 3])
        print('узоров: %d → %s' % (n, sys.argv[i + 3]))


if __name__ == '__main__':
    main()
