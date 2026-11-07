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
        self.jumps = set()
        self.done = False

    def restart(self, b, reload_delay):
        if reload_delay: b['delay'] = b['delay0']
        b['cur'] = b['start']; b['left'] = b['rows']

    def event(self):
        rd, w16 = C.rd, C.w16
        p = self.p; c = rd(p)
        self.events.append((self.pos, c, p))
        if c == 3:                                  # переход: позиция берётся из нового потока
            q = w16(p + 1); self.next = self.pos = w16(q); self.p = q + 2
            if q in self.jumps: self.done = True     # пошли по кругу — для картинки хватит
            self.jumps.add(q); return
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


# ---------------------------------------------------------------- картинки

NES_RGB = None


def nes_palette():
    """палитра NES из core/nes.c (NES_PALETTE)"""
    import re
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'core', 'nes.c')).read()
    body = src[src.index('NES_PALETTE[64]'):]
    body = body[body.index('{') + 1:body.index('}')]
    vals = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{6,8})', body)]
    return [((v >> 16) & 255, (v >> 8) & 255, v & 255) for v in vals[:64]]


def load_states(path):
    import numpy as np
    R = 9 + 2048 + 8192 + 32 + 256 * 240
    d = np.fromfile(path, dtype=np.uint8)
    out = []
    for i in range(len(d) // R):
        r = d[i * R:(i + 1) * R]
        out.append({'seg': int(r[1]) | int(r[2]) << 8, 'corr': int(r[3]), 'moving': (r[4], r[5]) != (r[6], r[7]),
                    'ctrl': int(r[8]), 'ram': r[9:9 + 2048], 'chr': r[9 + 2048:9 + 2048 + 8192], 'pal': r[9 + 2048 + 8192:9 + 2048 + 8192 + 32],
                    'screen': r[9 + 2048 + 8192 + 32:]})
    return out


class CellArt:
    """клетка 16×16 из состояния приставки: наборы $460/$468, первый тайл $470, 5 байт на клетку"""
    def __init__(self, st, rom):
        self.st, self.rom = st, rom
        self.pal = nes_palette()
        self.bgbase = 0x1000 if st['ctrl'] & 0x10 else 0
        self.cache = {}

    def byte(self, a):
        if a < 0x800: return int(self.st['ram'][a])
        if a >= 0xC000: return self.rom.byte(7, a)
        return self.rom.byte(5, a)

    def tile(self, t, palno, img, ox, oy):
        chr_ = self.st['chr']; base = self.bgbase + t * 16
        for y in range(8):
            lo, hi = int(chr_[base + y]), int(chr_[base + y + 8])
            for x in range(8):
                v = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1)
                c = int(self.st['pal'][0]) if v == 0 else int(self.st['pal'][palno * 4 + v])
                img.putpixel((ox + x, oy + y), self.pal[c & 63])

    def cell(self, v):
        from PIL import Image
        if v in self.cache: return self.cache[v]
        ram = self.st['ram']
        s, i = v >> 5, v & 31
        ptr = (int(ram[0x460 + s]) | int(ram[0x468 + s]) << 8) + 5 * i
        first = int(ram[0x470 + s])
        img = Image.new('RGB', (16, 16))
        tiles = [self.byte(ptr + k) for k in range(4)]
        palno = self.byte(ptr + 4) & 3
        for k, (ox, oy) in enumerate(((0, 0), (8, 0), (0, 8), (8, 8))):
            t = 0x20 if tiles[k] == 0xFF else (tiles[k] + first) & 0xFF
            self.tile(t, palno, img, ox, oy)
        self.cache[v] = img
        return img


def render(rom_path, ev_path, states_path, outdir, max_rows=1400, panel=64):
    from PIL import Image, ImageDraw
    from disasm import Rom
    rom = Rom(rom_path)
    os.makedirs(outdir, exist_ok=True)
    starts = {}
    for line in open(ev_path):
        q = line.split()
        if q[0] == 'B' and 1 <= int(q[3]) <= 22: starts.setdefault(int(q[3]), set()).add(int(q[1], 16))
    # поток, общий для многих коридоров, — вход/заставка, не сам коридор
    use = {}
    for c, ss in starts.items():
        for x in ss: use[x] = use.get(x, 0) + 1
    starts = {c: {x for x in ss if use[x] < 3} or ss for c, ss in starts.items()}
    states = load_states(states_path)
    made = []
    for corr in sorted(starts):
        # основной поток коридора — самый длинный
        best = None
        for start in starts[corr]:
            sim = Sim(start, corr); rows = []
            while len(rows) < max_rows:
                rows.append(sim.row()[1])
                if sim.done: break
            if best is None or len(rows) > len(best[1]): best = (start, rows, sim.events)
        start, rows, events = best
        # графика — из состояния, когда игра была внутри этого потока (не на входе в коридор)
        def inside(s):
            bc = int(s['ram'][0xBC]) | int(s['ram'][0xBD]) << 8
            return s['corr'] == corr and s['moving'] and start <= bc < start + 0x300
        st = next((s for s in states if inside(s)), None) or next((s for s in states if s['corr'] == corr and s['moving']), None)
        if st is None: continue
        art = CellArt(st, rom)
        npan = (len(rows) + panel - 1) // panel
        W, H = npan * (256 + 24), panel * 16 + 20
        img = Image.new('RGB', (W, H), (24, 24, 24))
        dr = ImageDraw.Draw(img)
        for r, cells in enumerate(rows):
            px, py = (r // panel) * (256 + 24) + 20, H - 4 - ((r % panel) + 1) * 16
            for c, v in enumerate(cells):
                img.paste(art.cell(v), (px + c * 16, py))
            if r % 16 == 0: dr.text((px - 19, py + 2), str(r), fill=(200, 200, 200))
        # события: волны — жёлтая черта слева, группа — голубая, босс — красная
        first = rows and Sim(start, corr).next
        for pos, c, addr in events:
            r = (pos - first) if first is not None else 0
            if r < 0 or r >= len(rows): continue
            col = {2: (240, 220, 0), 7: (80, 180, 255), 5: (255, 40, 40)}.get(c)
            if col is None: continue
            px, py = (r // panel) * (256 + 24) + 20, H - 4 - ((r % panel) + 1) * 16
            dr.rectangle((px - 4, py, px - 1, py + 15), fill=col)
        path = os.path.join(outdir, 'corridor_%02d.png' % corr)
        img.save(path)
        made.append((corr, start, len(rows), path))
    return made


if __name__ == '__main__' and '--png' in sys.argv:
    k = sys.argv.index('--png')
    for corr, start, n, path in render(sys.argv[1], sys.argv[2], sys.argv[k + 1], sys.argv[k + 2]):
        print('коридор %d: поток $%04X, %d рядов → %s' % (corr, start, n, path))
