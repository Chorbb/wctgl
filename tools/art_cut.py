#!/usr/bin/env python3
"""art_cut.py — нарезка перерисованного листа объектов на картинки для ядра (арт мода).

    python3 tools/art_cut.py лист_пользователя.png заготовка_x1.png объекты.json out_dir

Берутся только кадры, которые на листе изменены. Рисунок может выходить за рамку старого
кадра: область кадра — до середины промежутка к соседним кадрам (по ряду и по рядам).
Делает out_dir/art.txt и картинки t<тип>_f<кадр>.png. Строка art.txt:
    <тип hex> <кадр hex> <ax> <ay> <файл> <dx,dy спрайтов NES, которые убрать…>
ax, ay — где левый верхний угол картинки относительно X/Y объекта (как в буфере $0200).
"""
import sys, os, json
from PIL import Image, ImageChops


def main():
    user, tmpl, js, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    U = Image.open(user).convert('RGBA'); T = Image.open(tmpl).convert('RGBA')
    assert U.size == T.size, 'размер листа изменился: %s, а был %s — фигуры сдвинулись бы' % (U.size, T.size)
    L = json.load(open(js))
    rows = sorted({l['y'] for l in L})
    lines, report = [], []
    for l in L:
        same = sorted((m for m in L if m['y'] == l['y']), key=lambda m: m['x'])
        i = same.index(l)
        x0 = (same[i - 1]['x'] + same[i - 1]['w'] + l['x']) // 2 if i else 0
        x1 = (l['x'] + l['w'] + same[i + 1]['x']) // 2 if i + 1 < len(same) else U.width
        r = rows.index(l['y'])
        bottom_prev = max(m['y'] + m['h'] for m in L if m['y'] == rows[r - 1]) if r else 0
        y0 = (bottom_prev + l['y']) // 2 if r else 0
        bottom = max(m['y'] + m['h'] for m in same)
        y1 = (bottom + rows[r + 1]) // 2 if r + 1 < len(rows) else U.height
        box = (x0, y0, x1, y1)
        if not ImageChops.difference(U.crop(box), T.crop(box)).getbbox(): continue
        reg = U.crop(box)
        bb = reg.getchannel('A').point(lambda a: 255 if a >= 128 else 0).getbbox()
        if not bb: continue
        img = reg.crop(bb)
        name = 't%02X_f%02X.png' % (l['type'], l['frame'])
        img.save(os.path.join(out, name))
        ax = l['origin_dx'] + (x0 + bb[0] - l['x'])
        ay = l['origin_dy'] + (y0 + bb[1] - l['y'])
        hide = ' '.join('%d,%d' % (s['dx'], s['dy']) for s in l['sprites'])
        lines.append('%02X %02X %d %d %s %s' % (l['type'], l['frame'], ax, ay, name, hide))
        report.append('тип %02X кадр %02X: %d×%d (было %d×%d), сдвиг %d,%d' % (l['type'], l['frame'], img.width, img.height, l['w'], l['h'], ax, ay))
    # art.raw — то же одним файлом для тестов ядра (xtest XT_ART): число; на кадр: тип, кадр, ax, ay
    # (байты со знаком), w, h (по 2 байта), число спрятать, пары dx,dy, затем w*h*4 байт RGBA
    import struct
    raw = bytearray(struct.pack('<H', len(lines)))
    for ln in lines:
        q = ln.split()
        im = Image.open(os.path.join(out, q[4])).convert('RGBA')
        hide = [tuple(map(int, h.split(','))) for h in q[5:]]
        raw += struct.pack('<BBbbHHB', int(q[0], 16), int(q[1], 16), int(q[2]), int(q[3]), im.width, im.height, len(hide))
        for dx, dy in hide: raw += struct.pack('<bb', dx, dy)
        raw += im.tobytes()
    open(os.path.join(out, 'art.raw'), 'wb').write(raw)
    open(os.path.join(out, 'art.txt'), 'w').write('# тип кадр ax ay файл спрайты-NES-убрать (dx,dy)\n' + '\n'.join(lines) + '\n')
    print('\n'.join(report) or 'изменённых кадров нет')
    print('кадров: %d → %s' % (len(lines), out))


if __name__ == '__main__':
    main()
