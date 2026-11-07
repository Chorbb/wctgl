#!/usr/bin/env python3
"""corridor.py — расшифровка потока данных коридора (банк 4, интерпретатор $E6CC-$E8A8, банк 7).

    python3 tools/corridor.py rom.nes build/corr_ev.txt > build/corridors.md   # описание
    python3 tools/corridor.py rom.nes build/corr_ev.txt --check                # сверка с записью

Поток: слово «позиция первого события», затем события. Событие = команда (1 байт),
параметры, слово «позиция следующего события». Позиция — пройденный путь прокрутки
($B0/$B1, +1 за 16 точек). Когда путь доходит до позиции, выполняется команда.
Команды (по коду):
  0 скорость   слово → $BA/$BB (бит 15: ещё и $B8/$B9)
  1 фон        слово → $B4/$B5: указатель на последовательность узора фона
  2 волны      байт флагов → $60 (биты 0-1); если бит 1 — слово: сценарий волн (банк 4)
  3 переход    слово → новый поток (его позиция берётся оттуда же)
  4 загрузка   слово → $D92D (загрузка данных: палитра/графика)
  5 босс       $70 = 1; в коридоре 21 — ещё байт → $72
  6 объекты    N, затем N описаний крупных объектов в таблицы $400-$448
  7 группа     ждать конца прежней группы ($27), слово → $28/$29: группа врагов, банк 4
  8 параметры  пары (номер < 8, значение) → $199+номер, до байта ≥ 8
  9 повтор     если $70 (босс ещё жив) — переход по слову, иначе дальше
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disasm import Rom

rom = Rom(sys.argv[1])
B = 4
rd = lambda a: rom.byte(B, a & 0xFFFF)
w16 = lambda a: rd(a) | rd(a + 1) << 8
NAMES = ['скорость', 'фон', 'волны', 'переход', 'загрузка', 'босс', 'объекты', 'группа', 'параметры', 'повтор']


def entry(p, corridor, boss_alive=False):
    """событие по адресу p: (команда, параметры, длина до слова следующей позиции, адрес перехода или None)"""
    c = rd(p)
    if c == 0: return c, {'скорость': w16(p + 1)}, 2, None
    if c == 1: return c, {'узор': '$%04X' % w16(p + 1)}, 2, None
    if c == 2:
        f = rd(p + 1)
        if f & 2: return c, {'флаги': f, 'сценарий': '$%04X' % w16(p + 2)}, 3, None
        return c, {'флаги': f}, 1, None
    if c == 3: return c, {'поток': '$%04X' % w16(p + 1)}, None, w16(p + 1)
    if c == 4: return c, {'данные': '$%04X' % w16(p + 1)}, 2, None
    if c == 5:
        if corridor == 21: return c, {'$72': rd(p + 1)}, 1, None
        return c, {}, 0, None
    if c == 6:
        n = rd(p + 1); i = 2; objs = []
        if n == 0: return c, {'объекты': []}, 1, None
        for _ in range(n):
            f = rd(p + i); i += 1
            o = {'флаги': '$%02X' % f, 'слот': f & 7}
            if not f & 0x80:
                o['$430'] = rd(p + i); i += 1
                if not f & 0x20: o['$408'] = rd(p + i); i += 1
            if not f & 0x40: o['$438'] = rd(p + i); i += 1
            if not f & 0x20: o['$410'] = rd(p + i); i += 1
            o['$400'] = rd(p + i); i += 1
            if f & 0x10: o['данные'] = [rd(p + i + k) for k in range(4)]; i += 4
            else: o['указатель'] = '$%04X' % w16(p + i); i += 2
            objs.append(o)
        return c, {'объекты': objs}, i - 1, None
    if c == 7: return c, {'группа': '$%04X' % w16(p + 1)}, 2, None
    if c == 8:
        y = 1; pairs = []
        while rd(p + y) < 8: pairs.append((rd(p + y), rd(p + y + 1))); y += 2
        return c, {'пары': pairs}, y, None
    if c == 9:
        if boss_alive: return c, {'переход': '$%04X' % w16(p + 1)}, None, w16(p + 1)
        return c, {'если босс жив, переход': '$%04X' % w16(p + 1)}, 2, None
    return c, {'?': c}, 0, None


def walk(start, corridor, limit=300):
    """события потока по порядку (без ветвлений по «повтору»): [(позиция, адрес, команда, параметры)]"""
    pos, p, out, seen = w16(start), start + 2, [], set()
    while len(out) < limit:
        if p in seen: break
        seen.add(p)
        c, prm, k, jump = entry(p, corridor)
        out.append((pos, p, c, prm))
        if jump is not None:
            pos, p = w16(jump), jump + 2
            continue
        if c > 9: break
        pos, p = w16(p + k + 1), p + k + 3
    return out


# начала потоков и выполненные команды из записи
starts, runs, cur = {}, [], None
for line in open(sys.argv[2]):
    q = line.split()
    if q[0] == 'R': cur = None
    elif q[0] == 'B':
        st, corr = int(q[1], 16), int(q[3])
        starts.setdefault(st, corr)
        cur = {'start': st, 'corr': corr, 'cmds': []}; runs.append(cur)
    elif q[0] == 'C' and cur is not None:
        cur['cmds'].append((int(q[1]), int(q[2], 16)))

if '--check' in sys.argv:
    ok = bad = n = 0
    for r in runs:
        # идём по потоку, следуя туда же, куда пошла игра (переходы «повтора» — по факту)
        p, pos = r['start'] + 2, None
        good = True
        for c, addr in r['cmds']:
            if addr != p or rd(addr) != c:
                good = False
                print('расхождение: поток $%04X коридор %d — ожидалась команда по $%04X, была %d по $%04X'
                      % (r['start'], r['corr'], p, c, addr))
                break
            n += 1
            cc, prm, k, jump = entry(p, r['corr'])
            if jump is not None: p = jump + 2
            elif cc == 9:
                # «повтор»: куда пошла игра — узнаем по следующей команде
                nxt = r['cmds'][r['cmds'].index((c, addr)) + 1][1] if (c, addr) != r['cmds'][-1] else None
                p = (w16(p + 1) + 2) if nxt == w16(p + 1) + 2 else p + k + 3
            else: p = p + k + 3
        ok += good; bad += not good
    print('потоков сверено %d: совпало %d, расхождений %d; команд сверено %d' % (ok + bad, ok, bad, n))
    sys.exit(0)

print('# Коридоры: поток данных уровня\n')
print('Банк 4. Позиция — пройденный путь прокрутки (+1 за 16 точек). Расшифровано по коду; сверено с записью.\n')
for st, corr in sorted(starts.items(), key=lambda t: t[1]):
    print('## Коридор %d — поток $%04X\n' % (corr, st))
    print('| позиция | адрес | команда | параметры |')
    print('|---|---|---|---|')
    for pos, p, c, prm in walk(st, corr):
        print('| %d | $%04X | %s | %s |' % (pos, p, NAMES[c] if c < 10 else '?', ', '.join('%s: %s' % kv for kv in prm.items())))
    print()
