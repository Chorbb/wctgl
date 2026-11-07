#!/usr/bin/env python3
"""mod.py — сборка мода: понятные команды → список правок ROM (.tglpatch) для Godot.

    python3 tools/mod.py rom.nes mods/пример.tglmod build/пример.tglpatch

Файл мода (.tglmod) — по команде на строку, '#' — комментарий:

    здоровье <тип> <значение>                    таблица $D0AB (банк 7), тип 1..127
    урон <тип> <значение>                        урон при столкновении, $D12A
    урон_оружия <номер> <уровень 1-3> <значение> спецоружие, $86F5 (банк 6)
    волна <банк> <адрес> <n> <тип>               n-й враг/генератор сценария волн (с 1) → другой тип
    байт <банк> <адрес> <значение> [...]         любые байты (адрес процессора, $ — шестнадцатеричный)

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


def main():
    rom_path, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    rom = Rom(rom_path)
    waves.init(rom_path)
    code = code_bytes(os.path.join(os.path.dirname(dst) or '.', 'coverage.txt'), rom) or \
        code_bytes('build/coverage.txt', rom)
    patches, errors, touches_code = [], [], []
    for ln, raw in enumerate(open(src, encoding='utf-8'), 1):
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
            elif cmd in ('байт', 'byte'):
                bank, addr = num(a[0]), num(a[1])
                patches.append((bank, addr, [num(x) & 255 for x in a[2:]], raw.strip()))
            else:
                errors.append('строка %d: неизвестная команда «%s»' % (ln, w[0]))
        except (AssertionError, IndexError, ValueError) as e:
            errors.append('строка %d: %s — %s' % (ln, raw.strip(), e or 'неверные числа'))
    for bank, addr, data, text in patches:
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
            old = [rom.byte(bank, addr + k) for k in range(len(data))]
            f.write('# %s  (было %s)\n' % (text, ' '.join('%02X' % x for x in old)))
            f.write('%d %04X %s\n' % (bank, addr, ' '.join('%02X' % x for x in data)))
    print('правок: %d → %s' % (len(patches), dst))
    for t in touches_code:
        print('внимание: «%s» меняет код — в игре будет работать эмулятор (перевод выключится)' % t)


if __name__ == '__main__':
    main()
