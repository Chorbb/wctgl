#!/usr/bin/env python3
"""labyrinth_map.py — карта лабиринта из комнат, построенных игрой в записи.

    python3 tools/labyrinth_map.py rom.nes build/rooms.txt build/labyrinth.png [масштаб]

rooms.txt — от `XT_ROOM=1 xtest statwatch`: строки K (содержимое CHR) и M (комната: X, Y, зона,
адрес записи комнаты, 192 клетки $740-$7FF = 16×12, наборы клеток $460-$477, палитра).
Каждая комната рисуется той графикой, что была у игры в момент постройки. Сетка 24×24;
комнаты, где игрок не был, — пустые.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image, ImageDraw
from disasm import Rom
import corridor_map as CM


def load(path):
    chr_by_hash, rooms = {}, {}
    for line in open(path):
        if line.startswith('K '):
            _, h, data = line.split()
            chr_by_hash[h] = np.frombuffer(bytes.fromhex(data), dtype=np.uint8)
        elif line.startswith('M '):
            p = line.split()
            x, y, area, rec, h, ctrl = int(p[1]), int(p[2]), int(p[3]), int(p[4], 16), p[5], int(p[6], 16)
            cells = bytes.fromhex(p[7]); sets = bytes.fromhex(p[8]); pal = bytes.fromhex(p[9])
            if len(cells) != 192: continue
            if len(set(cells)) < 3: continue           # снято до постройки (пустая сетка)
            rooms[(x, y)] = dict(area=area, rec=rec, chr=h, ctrl=ctrl, cells=cells, sets=sets, pal=pal)
    return chr_by_hash, rooms


def room_image(rom, chr_by_hash, rm):
    ram = np.zeros(2048, dtype=np.uint8)
    ram[0x460:0x478] = list(rm['sets'])
    st = {'ram': ram, 'chr': chr_by_hash[rm['chr']], 'pal': np.frombuffer(rm['pal'], dtype=np.uint8),
          'ctrl': rm['ctrl']}
    art = CM.CellArt(st, rom)
    img = Image.new('RGB', (256, 192))
    for i, v in enumerate(rm['cells']):
        img.paste(art.cell(v), ((i % 16) * 16, (i // 16) * 16))
    return img


def main():
    rom = Rom(sys.argv[1])
    chr_by_hash, rooms = load(sys.argv[2])
    out = sys.argv[3]
    scale = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
    W, H = int(256 * scale), int(192 * scale)
    big = Image.new('RGB', (24 * W + 2 * 24, 24 * H + 2 * 24), (16, 16, 16))
    dr = ImageDraw.Draw(big)
    for (x, y), rm in rooms.items():
        if not (0 <= x < 24 and 0 <= y < 24): continue
        im = room_image(rom, chr_by_hash, rm).resize((W, H))
        big.paste(im, (x * (W + 2) + 1, y * (H + 2) + 1))
        dr.text((x * (W + 2) + 3, y * (H + 2) + 2), '%d,%d' % (x, y), fill=(255, 255, 0))
    big.save(out)
    print('комнат нарисовано: %d → %s' % (len(rooms), out))


if __name__ == '__main__':
    main()
