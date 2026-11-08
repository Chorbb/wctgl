#!/usr/bin/env python3
"""mod.py — сборка мода: понятные команды → список правок ROM (.tglpatch) для Godot.

    python3 tools/mod.py rom.nes mods/пример.tglmod build/пример.tglpatch

Файл мода (.tglmod) — по команде на строку, '#' — комментарий:

    здоровье <тип> <значение>                    таблица $D0AB (банк 7), тип 1..127
    урон <тип> <значение>                        урон при столкновении, $D12A
    урон_оружия <номер> <уровень 1-3> <значение> спецоружие, $86F5 (банк 6)
    волна <банк> <адрес> <n> <тип>               n-й враг/генератор сценария волн (с 1) → другой тип
    байт <банк> <адрес> <значение> [...]         любые байты (адрес процессора, $ — шестнадцатеричный)
    проход <X> <Y> <С|З|В|Ю> [закрыть]            лабиринт: открыть/закрыть проход из комнаты X,Y
                                                 (N/W/E/S тоже можно); правится и соседняя комната
    узор <X> <Y> <картинка.png>                  свой узор блоков комнаты: PNG 16×9, пиксель = клетка
    узор <X> <Y>                                 то же текстом: следующие 9 строк по 16 символов

Свой узор кладётся в память модов ядра (банк 64, см. core/custom.h) — без ограничений формата
игры. Цвета картинки и символы текста:
    чёрный     .  пусто (клетка шаблона комнаты)
    красный    к  красно-белый конус: не разбивается                     $09
    жёлтый     б  оливковый блок, 3 выстрела                             $01
    оранжевый  о  оливковый блок, 2 выстрела (выглядит так же)           $02
    коричневый л  оливковый блок, 1 выстрел (выглядит так же)            $03
    синий      г  голубой шар: пройти по нему — чипы                     $05
    серый      р  решётка: проходима, украшение                          $0A
    пурпур     ш  красная плитка: разбить выстрелом → голубой шар (чипы) $04
(проверено по всей записи игры: xtest blockwatch, см. PROGRESS.md, сессия 21)
Цвет берётся ближайший из таблицы; слишком непохожий — ошибка с координатами пикселя.

Правка попадает в копию ROM в памяти; исходный файл не меняется. Если правка задевает
исполняемый код (по build/coverage.txt), инструмент предупреждает и пишет в патч строку
«translation off»: переведённый код уже собран из ROM и такую правку не увидит, поэтому
Godot тогда исполняет игру эмулятором.

Формат .tglpatch: строки «<банк> <адрес hex> <байты hex…>», «# …», «translation off».
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import Rom, OPS, SIZE
import waves
import labyrinth


def num(s):
    s = s.strip()
    if s.startswith('$'): return int(s[1:], 16)
    if s.lower().startswith('0x'): return int(s, 16)
    return int(s)


def code_bytes(cov_path, rom):
    """байты ROM, которые исполнялись как код: {(банк, адрес)}"""
    out = set()
    if not os.path.exists(cov_path): return None
    for line in open(cov_path):
        p = line.split()
        if len(p) != 3 or p[0] != 'C': continue
        b, a = int(p[1]), int(p[2], 16)
        op = rom.byte(b, a)
        n = SIZE[OPS[op][1]] if op in OPS else 1
        for k in range(n): out.add((b, a + k))
    return out


BLOCKS = [  # (символ, цвет картинки, значение клетки)
    ('.', (0, 0, 0), 0x00), ('к', (255, 0, 0), 0x09), ('б', (255, 255, 0), 0x01),
    ('о', (255, 136, 0), 0x02), ('л', (136, 85, 0), 0x03),
    ('г', (0, 0, 255), 0x05), ('р', (128, 128, 128), 0x0A), ('ш', (255, 0, 255), 0x04),
]
MOD_BANK, PAT_SLOTS = 64, 80


def pattern_from_png(path):
    from PIL import Image
    im = Image.open(path).convert('RGB')
    assert im.size == (16, 9), 'картинка должна быть 16×9 пикселей, а она %d×%d' % im.size
    out, bad = [], []
    for y in range(9):
        for x in range(16):
            p = im.getpixel((x, y))
            d, v = min((sum((a - b) ** 2 for a, b in zip(p, c)), v) for _, c, v in BLOCKS)
            if d > 3 * 80 ** 2: bad.append('%d,%d #%02X%02X%02X' % (x, y, *p))
            out.append(v)
    assert not bad, 'непонятные цвета пикселей (x,y): ' + ', '.join(bad[:8])
    return out


def pattern_from_text(rows):
    sym = {c: v for c, _, v in BLOCKS}
    assert len(rows) == 9, 'нужно 9 строк узора'
    out = []
    for r in rows:
        assert len(r) == 16, 'в строке узора должно быть 16 символов: «%s»' % r
        for ch in r:
            assert ch in sym, 'непонятный символ «%s» в узоре' % ch
            out.append(sym[ch])
    return out


def main():
    rom_path, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    rom = Rom(rom_path)
    waves.init(rom_path)
    rooms, _ = labyrinth.read(rom)
    code = code_bytes(os.path.join(os.path.dirname(dst) or '.', 'coverage.txt'), rom) or \
        code_bytes('build/coverage.txt', rom)
    patches, errors, touches_code = [], [], []
    src_lines = list(enumerate(open(src, encoding='utf-8'), 1))
    room_pat = {}                                  # (x, y) → 144 клетки
    it = iter(src_lines)
    for ln, raw in it:
        line = raw.split('#', 1)[0].strip()
        if not line: continue
        w = line.split()
        cmd, a = w[0].lower(), w[1:]
        try:
            if cmd in ('здоровье', 'hp'):
                t, v = num(a[0]), num(a[1]); assert 1 <= t <= 127 and 0 <= v <= 255
                patches.append((7, 0xD0AB + t, [v], raw.strip()))
            elif cmd in ('урон', 'damage'):
                t, v = num(a[0]), num(a[1]); assert 1 <= t <= 127 and 0 <= v <= 255
                patches.append((7, 0xD12A + t, [v], raw.strip()))
            elif cmd in ('урон_оружия', 'weapon_damage'):
                wn, lv, v = num(a[0]), num(a[1]), num(a[2]); assert 0 <= wn <= 10 and 1 <= lv <= 3 and 0 <= v <= 255
                patches.append((6, 0x86F5 + 11 * lv + wn, [v], raw.strip()))
            elif cmd in ('волна', 'wave'):
                bank, addr, n, t = num(a[0]), num(a[1]), num(a[2]), num(a[3])
                items = [e for e in waves.decode(bank, addr) if e[0] in ('враг', 'генератор')]
                assert 1 <= n <= len(items), 'в сценарии всего %d врагов' % len(items)
                kind, _, taddr = items[n - 1]
                assert 3 <= t <= 127, 'тип врага 3..127'
                patches.append((bank, taddr, [t | (0x80 if kind == 'генератор' else 0)], raw.strip()))
            elif cmd in ('проход', 'door'):
                x, y = num(a[0]), num(a[1])
                d = {'с': 'N', 'з': 'W', 'в': 'E', 'ю': 'S'}.get(a[2].lower(), a[2].upper())
                close = len(a) > 3 and a[3].lower() in ('закрыть', 'close')
                dd = {k: (bit, dx, dy, opp) for k, bit, dx, dy, opp in labyrinth.DOORS}
                assert d in dd, 'направление: С, З, В или Ю'
                bit, dx, dy, opp = dd[d]
                for (rx, ry, b) in ((x, y, bit), (x + dx, y + dy, opp)):
                    rm = rooms.get((rx, ry))
                    assert rm and rm['exists'] and len(rm['raw']) > 1, 'комнаты %d,%d нет' % (rx, ry)
                    v = rm['raw'][1] & ~b if close else rm['raw'][1] | b
                    rm['raw'][1] = v
                    patches.append((5, rm['addr'] + 1, [v], raw.strip()))
            elif cmd in ('узор', 'pattern'):
                x, y = num(a[0]), num(a[1])
                assert 0 <= x < 24 and 0 <= y < 24, 'комната вне карты 24×24'
                rm = rooms[(x, y)]
                assert rm['exists'] and not rm['special'], 'в %d,%d нет обычной комнаты' % (x, y)
                if len(a) > 2:
                    png = a[2] if os.path.isabs(a[2]) else os.path.join(os.path.dirname(src), a[2])
                    room_pat[(x, y)] = pattern_from_png(png)
                else:
                    rows = []
                    while len(rows) < 9:
                        _, r2 = next(it)
                        r2 = r2.split('#', 1)[0].strip()
                        if r2: rows.append(r2)
                    room_pat[(x, y)] = pattern_from_text(rows)
                assert len(room_pat) <= PAT_SLOTS, 'своих узоров не больше %d' % PAT_SLOTS
            elif cmd in ('байт', 'byte'):
                bank, addr = num(a[0]), num(a[1])
                patches.append((bank, addr, [num(x) & 255 for x in a[2:]], raw.strip()))
            else:
                errors.append('строка %d: неизвестная команда «%s»' % (ln, w[0]))
        except (AssertionError, IndexError, ValueError) as e:
            errors.append('строка %d: %s — %s' % (ln, raw.strip(), e or 'неверные числа'))
    for n, ((x, y), cells) in enumerate(sorted(room_pat.items()), 1):
        patches.append((MOD_BANK, 24 * y + x, [n], 'узор %d %d → свой узор %d' % (x, y, n)))
        patches.append((MOD_BANK, 0x400 + 144 * (n - 1), cells, 'клетки своего узора %d' % n))
    for bank, addr, data, text in patches:
        if bank == MOD_BANK:
            if not (0 <= addr and addr + len(data) <= 0x4000): errors.append('%s: вне памяти модов' % text)
            continue
        base = 0xC000 if bank == 7 else 0x8000
        if not (base <= addr and addr + len(data) <= base + 0x4000):
            errors.append('%s: адрес $%04X вне банка %d' % (text, addr, bank))
        if code and any((bank, addr + k) in code for k in range(len(data))):
            touches_code.append(text)
    if errors:
        for e in errors: print('ошибка:', e)
        sys.exit(1)
    with open(dst, 'w', encoding='utf-8') as f:
        f.write('# собрано tools/mod.py из %s\n' % os.path.basename(src))
        if touches_code:
            f.write('translation off\n')
        for bank, addr, data, text in patches:
            if bank == MOD_BANK:
                f.write('# %s  (память модов ядра)\n' % text)
            else:
                old = [rom.byte(bank, addr + k) for k in range(len(data))]
                f.write('# %s  (было %s)\n' % (text, ' '.join('%02X' % x for x in old)))
            f.write('%d %04X %s\n' % (bank, addr, ' '.join('%02X' % x for x in data)))
    print('правок: %d → %s' % (len(patches), dst))
    for t in touches_code:
        print('внимание: «%s» меняет код — в игре будет работать эмулятор (перевод выключится)' % t)


if __name__ == '__main__':
    main()
