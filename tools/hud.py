"""hud.py — разбор нижней панели The Guardian Legend из видеопамяти (экран 1, строки 25-27).

Панель пишется текстом ASCII:  'SC  38180  CHIP 100  SHOT  0' / '#10!!!!!!!!!' / '  AREA  1  X 6 Y10'.
Возвращает словарь чисел или None, если панели нет (титры, меню).
"""
import re


def parse_hud(nt):
    rows = [bytes(nt[1024 + r * 32:1024 + r * 32 + 32]) for r in (25, 26, 27)]
    txt = [''.join(chr(b) if 32 <= b < 127 else '~' for b in r) for r in rows]
    out = {}
    m = re.search(r'SC\s*(\d+)', txt[0])
    if not m:
        return None
    out['score'] = int(m.group(1))
    m = re.search(r'CHIP\s*(\d+)', txt[0])
    if m: out['chips'] = int(m.group(1))
    m = re.search(r'SHOT\s*(\d+)', txt[0])
    if m: out['shot'] = int(m.group(1))
    m = re.search(r'#\s*(\d+)', txt[1])
    if m: out['life_num'] = int(m.group(1))
    bar = rows[1][3:]
    out['life_bar'] = sum(1 for b in bar if b == 0x21)
    m = re.search(r'AREA\s*(\d+)', txt[2])
    if m: out['area'] = int(m.group(1))
    m = re.search(r'X\s*(\d+)', txt[2])
    if m: out['x'] = int(m.group(1))
    m = re.search(r'Y\s*(\d+)', txt[2])
    if m: out['y'] = int(m.group(1))
    out['_text'] = txt
    return out
