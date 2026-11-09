#!/usr/bin/env python3
"""sprite_sheet.py — заготовки для перерисовки спрайтов: каждый объект целиком, все его кадры.

    python3 tools/sprite_sheet.py build/sprites_c0.txt out_dir

Вход — `xtest sprites` (кадры объектов: спрайты относительно X/Y объекта, CHR, палитра).
Одинаковые кадры с разной палитрой (мигание при попадании) сводятся в один, палитра — самая
частая; из вариантов одного кадра берётся самый частый среди самых полных (без обрезки краем).
Делает:
  объекты_x1.png  — заготовка для рисования, 1:1: ряд на тип объекта, кадры слева направо,
                    между кадрами 8 пикселей прозрачного поля; рисовать поверх, не сдвигая;
  объекты_x4.png  — то же крупно, с подписями (тип, кадр, сколько раз встречается) — смотреть;
  объекты.json    — где на листе каждый кадр и из каких спрайтов/плиток он состоит (для обратной
                    нарезки готового рисунка на плитки).
"""
import sys, os, json
from collections import defaultdict
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tile_sheet import nes_palette


def parse(path):
    groups, cur, n = [], None, 0
    for line in open(path):
        q = line.split()
        if not q: continue
        if q[0] == 'N': n = int(q[1])
        elif q[0] == 'G':
            cur = dict(type=int(q[1], 16), frame=int(q[2], 16), tall=q[4] == '8x16', n=n, spr=[]); groups.append(cur)
        elif q[0] == 'F':
            cur['spr'].append((int(q[1]), int(q[2]), int(q[3], 16), q[4], q[5]))
    return groups


def choose(groups):
    by = defaultdict(lambda: defaultdict(lambda: [0, defaultdict(int)]))
    for g in groups:
        comp = tuple(sorted((dx, dy, at, ch) for dx, dy, at, ch, pal in g['spr']))
        rec = by[(g['type'], g['frame'], g['tall'])][comp]
        rec[0] += g['n']
        rec[1][tuple(pal for *_, pal in sorted(g['spr'], key=lambda s: s[:4]))] += g['n']
    out = []
    for (t, fr, tall), comps in by.items():
        most = max(len(c) for c in comps)
        best = max((c for c in comps if len(c) == most), key=lambda c: comps[c][0])
        pals = max(comps[best][1], key=comps[best][1].get)
        total = sum(v[0] for v in comps.values())
        out.append(dict(type=t, frame=fr, tall=tall, count=total,
                        spr=[dict(dx=dx, dy=dy, attr=at, chr=ch, pal=p) for (dx, dy, at, ch), p in zip(best, pals)]))
    return sorted(out, key=lambda f: (f['type'], f['frame']))


def draw_frame(fr, rgb):
    xs = [s['dx'] for s in fr['spr']]; ys = [s['dy'] for s in fr['spr']]
    h = 16 if fr['tall'] else 8
    x0, y0 = min(xs), min(ys)
    w, hh = max(xs) + 8 - x0, max(ys) + h - y0
    im = Image.new('RGBA', (w, hh), (0, 0, 0, 0))
    for s in fr['spr']:
        b = bytes.fromhex(s['chr'].replace('-', ''))
        pal = bytes.fromhex(s['pal'])
        t = Image.new('RGBA', (8, h), (0, 0, 0, 0))
        for half in range(h // 8):
            for y in range(8):
                for x in range(8):
                    lo, hi = b[half * 16 + y], b[half * 16 + y + 8]
                    c = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1)
                    if c: t.putpixel((x, half * 8 + y), rgb[pal[c - 1] & 63] + (255,))
        if s['attr'] & 0x40: t = t.transpose(Image.FLIP_LEFT_RIGHT)
        if s['attr'] & 0x80: t = t.transpose(Image.FLIP_TOP_BOTTOM)
        im.alpha_composite(t, (s['dx'] - x0, s['dy'] - y0))
    return im, x0, y0


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rgb = nes_palette()
    frames = choose(parse(src))
    rows = defaultdict(list)
    for f in frames: rows[f['type']].append(f)
    gap = 8
    layout, y = [], gap
    imgs = {}
    for t in sorted(rows):
        x, rh = gap, 0
        for f in rows[t]:
            im, x0, y0 = draw_frame(f, rgb)
            imgs[(t, f['frame'])] = (im, x, y)
            layout.append(dict(type=t, frame=f['frame'], x=x, y=y, origin_dx=x0, origin_dy=y0, w=im.width, h=im.height,
                               count=f['count'], tall=f['tall'], sprites=f['spr']))
            x += max(im.width, 30) + gap; rh = max(rh, im.height)   # 30 — место под подпись на листе ×4
        y += rh + gap + 8
    W = max(l['x'] + l['w'] for l in layout) + gap
    sheet = Image.new('RGBA', (W, y), (0, 0, 0, 0))
    for im, x, yy in imgs.values(): sheet.alpha_composite(im, (x, yy))
    sheet.save(os.path.join(out, 'объекты_x1.png'))
    big = Image.new('RGBA', (W * 4, y * 4), (48, 48, 48, 255))
    big.alpha_composite(sheet.resize((W * 4, y * 4), Image.NEAREST))
    dr = ImageDraw.Draw(big)
    try: font = ImageFont.truetype('DejaVuSans.ttf', 14)
    except OSError: font = None
    for l in layout:
        dr.rectangle((l['x'] * 4 - 1, l['y'] * 4 - 1, (l['x'] + l['w']) * 4, (l['y'] + l['h']) * 4), outline=(90, 90, 90))
        dr.text((l['x'] * 4, (l['y'] + l['h']) * 4 + 3), 'тип %02X кадр %02X' % (l['type'], l['frame']), fill=(220, 220, 220), font=font)
        dr.text((l['x'] * 4, (l['y'] + l['h']) * 4 + 19), '×%d' % l['count'], fill=(150, 150, 150), font=font)
    big.save(os.path.join(out, 'объекты_x4.png'))
    json.dump(layout, open(os.path.join(out, 'объекты.json'), 'w'), ensure_ascii=False, indent=1)
    print('объектов %d, кадров %d → %s' % (len(rows), len(layout), out))


if __name__ == '__main__':
    main()
