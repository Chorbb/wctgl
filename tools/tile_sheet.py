#!/usr/bin/env python3
"""tile_sheet.py — лист плиток 8×8, которые игра показывает (из `xtest tiles`), для перерисовки.

    python3 tools/tile_sheet.py build/tiles_c0.txt out_dir [масштаб=2]

Плитка = рисунок (16 байт CHR); у одной плитки бывает несколько палитр (затемнение, вспышки) —
на листе она одна, с самой частой палитрой. Делает:
  фон_xN.png, спрайты_xN.png — плитки сеткой 16 в ряд, каждая N×8 пикселей, между ними 2·N
  пикселей серой рамки; порядок — по тому, как часто плитка на экране (частые — первыми);
  плитки.txt — номер плитки на листе → её рисунок CHR (по нему ядро узнаёт плитку).
"""
import sys, os, re
from collections import defaultdict
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def nes_palette():
    src = open(os.path.join(HERE, '..', 'core', 'nes.c')).read()
    body = src[src.index('NES_PALETTE[64]'):]
    body = body[body.index('{') + 1:body.index('}')]
    return [((v >> 16) & 255, (v >> 8) & 255, v & 255) for v in (int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{6})', body))]


def load(path):
    d = defaultdict(lambda: defaultdict(int))
    for line in open(path):
        q = line.split()
        if len(q) == 5 and q[0] == 'T': d[(q[1], q[2])][q[3]] += int(q[4])
    return d


def tile_img(chr_hex, pal_hex, rgb, transparent):
    b = bytes.fromhex(chr_hex); pal = bytes.fromhex(pal_hex)
    im = Image.new('RGBA', (8, 8))
    for y in range(8):
        for x in range(8):
            c = ((b[y] >> (7 - x)) & 1) | (((b[y + 8] >> (7 - x)) & 1) << 1)
            im.putpixel((x, y), (0, 0, 0, 0) if (c == 0 and transparent) else rgb[pal[c] & 63] + (255,))
    return im


def main():
    path, out = sys.argv[1], sys.argv[2]
    sc = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    os.makedirs(out, exist_ok=True)
    rgb = nes_palette()
    d = load(path)
    lines = []
    for kind, name in (('b', 'фон'), ('s', 'спрайты')):
        tiles = sorted(((sum(p.values()), c, max(p, key=p.get)) for (k, c), p in d.items() if k == kind), reverse=True)
        cols, cell, gap = 16, 8 * sc, 2 * sc
        rows = (len(tiles) + cols - 1) // cols
        sheet = Image.new('RGBA', (cols * (cell + gap) + gap, rows * (cell + gap) + gap), (90, 90, 90, 255))
        for i, (n, c, pal) in enumerate(tiles):
            im = tile_img(c, pal, rgb, kind == 's').resize((cell, cell), Image.NEAREST)
            x, y = gap + (i % cols) * (cell + gap), gap + (i // cols) * (cell + gap)
            if kind == 's': sheet.paste((0, 0, 0, 0), (x, y, x + cell, y + cell))
            sheet.alpha_composite(im, (x, y))
            lines.append('%s %d %s' % (kind, i, c))
        sheet.save(os.path.join(out, '%s_x%d.png' % (name, sc)))
        print('%s: %d плиток' % (name, len(tiles)))
    open(os.path.join(out, 'плитки.txt'), 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
