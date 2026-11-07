#!/usr/bin/env python3
"""corridor_map.py — коридор целиком: ряды клеток по правилам игры и картинка.

    python3 tools/corridor_map.py rom.nes build/corr_ev.txt --check build/corr_rows.txt
        сверить построенные ряды с рядами из записи (xtest statwatch XT_CORR=1, строки W)
    python3 tools/corridor_map.py rom.nes build/corr_ev.txt --png build/states.bin build/maps/
        картинки коридоров (графика клеток — из состояния приставки в этом коридоре)

Ряд строится как в игре ($E6B3-$E974): путь +1; события потока с этой позиции; ряд узора
фона ($B4: указатель, число рядов, ряды по 16 клеток по кругу); поверх — 8 кистей.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corridor as C


def s8(v):
    return v - 256 if v > 127 else v


class Sim:
    def __init__(self, start, corridor):
        self.corr = corridor
        self.next = C.w16(start)
        self.p = start + 2
        self.pos = (self.next - 1) & 0xFFFF
        self.bg = None
        self.a1 = 0
        self.boss = 0
        self.brush = [dict(col=0x80) for _ in range(8)]
        self.events = []
        self.done = False

    def restart(self, b, reload_delay):
        if reload_delay: b['delay'] = b['delay0']
        b['cur'] = b['start']; b['left'] = b['rows']

    def event(self):
        rd, w16 = C.rd, C.w16
        p = self.p; c = rd(p)
        self.events.append((self.pos, c, p))
        if c == 3:                                  # переход: позиция берётся из нового потока
            q = w16(p + 1); self.next = self.pos = w16(q); self.p = q + 2; return
        if c == 9 and self.boss:                    # повтор, пока жив босс
            q = w16(p + 1); self.next = self.pos = w16(q); self.p = q + 2; self.done = True; return
        _, prm, k, _ = C.entry(p, self.corr)
        if c == 1: self.bg = w16(p + 1); self.a1 = 0 if False else self.a1
        if c == 5: self.boss = 1
        if c == 6:
            i = 2
            for _ in range(rd(p + 1)):
                f = rd(p + i); i += 1
                b = self.brush[f & 7]
                if f & 0x80: b['delay'] = 0; b['delay0'] = 0
                else:
                    b['delay'] = rd(p + i); i += 1
                    if f & 0x20: b['delay0'] = b['delay']
                    else: b['delay0'] = rd(p + i); i += 1
                if f & 0x40: b['dx'] = 0
                else: b['dx'] = rd(p + i); i += 1
                if f & 0x20: b['reps'] = 1
                else: b['reps'] = rd(p + i); i += 1
                b['col'] = rd(p + i); i += 1
                if f & 0x10:
                    b['start'] = rd(p + i) | rd(p + i + 1) << 8; b['cells'] = rd(p + i + 2); b['rows'] = rd(p + i + 3); i += 4
                else:
                    q = w16(p + i); b['cells'] = rd(q); b['rows'] = rd(q + 1); b['start'] = q + 2; i += 2
                self.restart(b, False)
        self.next = w16(p + k + 1); self.p = p + k + 3

    def row(self):
        """следующий ряд из 16 клеток"""
        rd = C.rd
        self.pos = (self.pos + 1) & 0xFFFF
        guard = 0
        while self.pos == self.next and guard < 64:
            self.event(); guard += 1
        cells = [0] * 16
        if self.bg is not None:
            cnt = rd(self.bg + 2)
            if self.a1 >= cnt: self.a1 = 0
            r = self.a1; self.a1 += 1
            base = (rd(self.bg) | rd(self.bg + 1) << 8) + 16 * r
            cells = [rd(base + k) for k in range(16)]
        for b in self.brush:
            if b['col'] == 0x80: continue
            if b['delay']: b['delay'] -= 1; continue
            x, y, w = b['col'], 0, b['cells']
            while True:
                v = rd(b['cur'] + y)
                if v == 0: x = (x + rd(b['cur'] + y + 1)) & 0xFF; y += 2; continue
                y += 1
                if v == 1: break
                if x < 16: cells[x] = v
                x = (x + 1) & 0xFF; w = (w - 1) & 0xFF
                if w == 0: break
            b['cur'] += y
            b['left'] = (b['left'] - 1) & 0xFF
            if b['left']: continue
            if b['reps']:
                b['reps'] -= 1
                if b['reps'] == 0: b['col'] = 0x80; continue
            b['col'] = (b['col'] + b['dx']) & 0xFF
            if b['col'] < 0x20 or b['col'] >= 0xF0: self.restart(b, True)
            else: b['col'] = 0x80
        return self.pos, cells


def load_runs(path):
    runs, cur = [], None
    for line in open(path):
        q = line.split()
        if q[0] == 'R': cur = None
        elif q[0] == 'B': cur = {'start': int(q[1], 16), 'corr': int(q[3]), 'rows': []}; runs.append(cur)
        elif q[0] == 'W' and cur is not None: cur['rows'].append((int(q[1]), [int(x, 16) for x in q[2:18]]))
    return runs


def check(rows_path):
    ok = bad = n = 0
    for r in load_runs(rows_path):
        if not r['rows']: continue
        sim = Sim(r['start'], r['corr'])
        good = True
        for pos, cells in r['rows']:
            sp, sc = sim.row()
            n += 1
            if (sp, sc) != (pos, cells):
                good = False
                print('коридор %d поток $%04X: ряд %d — построено %s, в игре %s (путь %d/%d)' %
                      (r['corr'], r['start'], n, ' '.join('%02X' % c for c in sc), ' '.join('%02X' % c for c in cells), sp, pos))
                break
        ok += good; bad += not good
    print('запусков сверено %d: совпало %d, расхождений %d; рядов сверено %d' % (ok + bad, ok, bad, n))


if __name__ == '__main__':
    C.init(sys.argv[1])
    if '--check' in sys.argv:
        check(sys.argv[sys.argv.index('--check') + 1])
