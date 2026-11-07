#!/usr/bin/env python3
"""xlat.py — переводчик процедур 6502 из ROM The Guardian Legend в C.

    python3 tools/xlat.py <rom.nes> <coverage.txt> <routines.txt> <out.c> [--closure]

coverage.txt — какие инструкции реально исполнялись (делает `xtest coverage`): строки
"C <банк> <адрес>". Переводятся только они; любой переход в непроверенный код отдаёт
управление эмулятору — так перевод никогда не «угадывает» код, которого не видел.

routines.txt — какие процедуры переводить: строки "<банк> <адрес> jsr|irq [имя]".
С --closure добавляются все процедуры, которые они вызывают (по JSR), рекурсивно.

Результат зависит от ROM, поэтому пишется в build/gen и в репозиторий не кладётся.
Семантика каждой инструкции и порядок тактов шины совпадают с core/cpu6502.c.
"""
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from disasm import Rom, OPS, SIZE  # noqa: E402

READ_OPS = {'LDA', 'LDX', 'LDY', 'ADC', 'SBC', 'AND', 'ORA', 'EOR', 'CMP', 'CPX', 'CPY', 'BIT'}
WRITE_OPS = {'STA', 'STX', 'STY'}
RMW_OPS = {'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'}
BRANCHES = {'BPL': ('0x80', False), 'BMI': ('0x80', True), 'BVC': ('0x40', False), 'BVS': ('0x40', True),
            'BCC': ('0x01', False), 'BCS': ('0x01', True), 'BNE': ('0x02', False), 'BEQ': ('0x02', True)}
FLAG_OPS = {'CLC': 'XP &= (uint8_t)~0x01;', 'SEC': 'XP |= 0x01;', 'CLV': 'XP &= (uint8_t)~0x40;',
            'CLD': 'XP &= (uint8_t)~0x08;', 'SED': 'XP |= 0x08;'}
IFLAG_OPS = {'CLI': 'XP &= (uint8_t)~0x04;', 'SEI': 'XP |= 0x04;'}
REG_OPS = {'TAX': 'XX = XA; XZN(XX);', 'TAY': 'XY = XA; XZN(XY);', 'TXA': 'XA = XX; XZN(XA);',
           'TYA': 'XA = XY; XZN(XA);', 'TSX': 'XX = XS; XZN(XX);', 'TXS': 'XS = XX;',
           'INX': 'XX++; XZN(XX);', 'INY': 'XY++; XZN(XY);', 'DEX': 'XX--; XZN(XX);', 'DEY': 'XY--; XZN(XY);',
           'NOP': ''}
ACC_OPS = {'ASL': 'XA = xlat_asl(n, XA);', 'LSR': 'XA = xlat_lsr(n, XA);',
           'ROL': 'XA = xlat_rol(n, XA);', 'ROR': 'XA = xlat_ror(n, XA);'}

# такты: (кол-во, маска записи) — как в core/cpu6502.c
READ_CYC = {'imm': (2, 0), 'zp': (3, 0), 'zpx': (4, 0), 'zpy': (4, 0), 'abs': (4, 0),
            'absx': (4, 0), 'absy': (4, 0), 'indx': (6, 0), 'indy': (5, 0)}
WRITE_CYC = {'zp': (3, 0b100), 'zpx': (4, 0b1000), 'zpy': (4, 0b1000), 'abs': (4, 0b1000),
             'absx': (5, 0b10000), 'absy': (5, 0b10000), 'indx': (6, 0b100000), 'indy': (6, 0b100000)}
RMW_CYC = {'zp': (5, 0b11000), 'zpx': (6, 0b110000), 'abs': (6, 0b110000), 'absx': (7, 0b1100000)}


def h4(v):
    return '0x%04X' % v


class Gen:
    def __init__(self, rom, coverage):
        self.rom = rom
        self.cov = coverage            # set of (bank, addr)
        self.nonproc = set()           # адреса переходников (не обычных процедур)

    def bank_of(self, addr, bank):
        return bank if 0x8000 <= addr < 0xC000 else 7

    def covered(self, bank, addr):
        return (self.bank_of(addr, bank), addr) in self.cov

    def decode(self, bank, pc):
        op = self.rom.byte(bank, pc)
        if op not in OPS:
            return None
        mn, mode = OPS[op]
        size = SIZE[mode]
        raw = [self.rom.byte(bank, (pc + i) & 0xFFFF) for i in range(size)]
        v = raw[1] if size == 2 else (raw[1] | raw[2] << 8) if size == 3 else 0
        return mn, mode, size, v

    def body(self, bank, entry):
        """Инструкции процедуры: обход от входа по исполнявшемуся коду."""
        seen, todo, calls = {}, [entry], set()
        while todo:
            pc = todo.pop()
            if pc in seen or not (pc >= 0x8000) or not self.covered(bank, pc):
                continue
            d = self.decode(bank, pc)
            seen[pc] = d
            if d is None:
                continue
            mn, mode, size, v = d
            nxt = (pc + size) & 0xFFFF
            if mn in BRANCHES:
                todo += [nxt, (nxt + (v - 256 if v > 127 else v)) & 0xFFFF]
            elif mn == 'JSR':
                calls.add(v)
                todo.append(nxt)
            elif mn == 'JMP' and mode == 'abs':
                todo.append(v)
            elif mn in ('RTS', 'RTI', 'BRK') or (mn == 'JMP' and mode == 'ind'):
                pass
            else:
                todo.append(nxt)
            if len(seen) > 3000:
                break
        return seen, calls

    def is_procedure(self, bank, entry, kind):
        """Обычная ли это процедура: стек внутри неё сбалансирован и выход — по RTS (RTI)
        на своём уровне. Переходники, которые снимают адрес возврата со стека
        (дальние вызовы между банками, параметры после JSR), так не устроены —
        их исполняет эмулятор, а вызов к ним отдаётся ему целиком."""
        body, _ = self.body(bank, entry)
        depth, todo = {}, [(entry, 0)]
        end = 'RTI' if kind == 'irq' else 'RTS'
        while todo:
            pc, dep = todo.pop()
            if pc not in body or body[pc] is None:
                continue
            if pc in depth:
                if depth[pc] != dep:
                    return False
                continue
            depth[pc] = dep
            mn, mode, size, v = body[pc]
            nxt = (pc + size) & 0xFFFF
            if mn in ('PHA', 'PHP'):
                dep += 1
            elif mn in ('PLA', 'PLP'):
                dep -= 1
                if dep < 0:
                    return False
            elif mn == 'TXS' or mn.endswith('*') and mn[:-1] in ('TAS', 'TXS'):
                return False
            if mn in ('RTS', 'RTI'):
                if mn != end or dep != 0:
                    return False
                continue
            if mn in BRANCHES:
                todo += [(nxt, dep), ((nxt + (v - 256 if v > 127 else v)) & 0xFFFF, dep)]
            elif mn == 'JMP' and mode == 'abs':
                todo.append((v, dep))
            elif mn in ('BRK',) or (mn == 'JMP' and mode == 'ind'):
                continue
            else:
                todo.append((nxt, dep))
        return True

    # -------------------------------------------------------------- адресация
    def ea(self, mode, v):
        """C-выражения: (подготовка, адрес, базовый адрес для пустого чтения, нужна ли проверка железа)."""
        if mode == 'zp':
            return '', h4(v), None, False
        if mode == 'zpx':
            return '', '(uint8_t)(0x%02X + XX)' % v, None, False
        if mode == 'zpy':
            return '', '(uint8_t)(0x%02X + XY)' % v, None, False
        if mode == 'abs':
            return '', h4(v), None, False
        if mode in ('absx', 'absy'):
            r = 'XX' if mode == 'absx' else 'XY'
            pre = 'uint16_t b_ = %s, a_ = (uint16_t)(b_ + %s); ' % (h4(v), r)
            risky = not (v + 255 < 0x2000 or v >= 0x4020)
            return pre, 'a_', 'b_', risky
        if mode == 'indx':
            pre = 'uint8_t z_ = (uint8_t)(0x%02X + XX); uint16_t a_ = (uint16_t)(XZP(z_) | (XZP((uint8_t)(z_ + 1)) << 8)); ' % v
            return pre, 'a_', None, True
        if mode == 'indy':
            pre = ('uint16_t b_ = (uint16_t)(XZP(0x%02X) | (XZP(0x%02X) << 8)), a_ = (uint16_t)(b_ + XY); '
                   % (v, (v + 1) & 0xFF))
            return pre, 'a_', 'b_', True
        raise ValueError(mode)

    def instr(self, bank, pc, d, fn_name, body):
        """C-код одной инструкции (без метки)."""
        mn, mode, size, v = d
        nxt = (pc + size) & 0xFFFF
        out = []

        def goto(addr):
            if addr in body and body[addr] is not None:
                return 'goto L_%04X;' % addr
            return 'xlat_bail_to(n, %s); return;' % h4(addr)

        def io_static(addr):
            return 0x2000 <= addr < 0x4020

        if mn.endswith('*'):                         # неофициальные — эмулятором
            out.append('xlat_io(n, %s, %s); %s' % (h4(pc), h4(nxt), goto(nxt)))
            return out

        # ---- управление
        if mn in BRANCHES:
            mask, setv = BRANCHES[mn]
            to = (nxt + (v - 256 if v > 127 else v)) & 0xFFFF
            cond = '(XP & %s)' % mask if setv else '!(XP & %s)' % mask
            out.append('if (%s) { xlat_branch(n, 1, %s, %s); %s }' % (cond, h4(nxt), h4(to), goto(to)))
            out.append('xlat_branch(n, 0, %s, %s); %s' % (h4(nxt), h4(to), goto(nxt)))
            return out
        if mn == 'JMP' and mode == 'abs':
            out.append('XCYC(3, 0, %s);' % h4(v))
            if v in body and body[v] is not None:
                out.append('goto L_%04X;' % v)
            else:
                out.append('xlat_jump(n, %s); return;' % h4(v))
            return out
        if mn == 'JSR' and v in self.nonproc:
            out.append('xlat_bail_to(n, %s); return;' % h4(pc))
            return out
        if mn == 'JSR':
            ret = (pc + 2) & 0xFFFF
            out.append('XPUSH(%s); XPUSH(%s); XCYC(6, 0b11000, %s);' % (h4(ret >> 8), h4(ret & 0xFF), h4(v)))
            out.append('if (!xlat_call(n, %s, %s)) return; %s' % (h4(v), h4(nxt), goto(nxt)))
            return out
        if mn == 'RTS':
            out.append('{ uint8_t lo_ = XPULL(); uint8_t hi_ = XPULL(); uint16_t r_ = (uint16_t)((lo_ | (hi_ << 8)) + 1);'
                       ' XCYC(6, 0, r_); } return;')
            return out
        if mn == 'RTI':
            out.append('{ xlat_tick(n, 4, 0); XP = (uint8_t)((XPULL() & ~0x10) | 0x20); uint8_t lo_ = XPULL(); '
                       'uint8_t hi_ = XPULL(); uint16_t r_ = (uint16_t)(lo_ | (hi_ << 8)); n->cpu.pc = r_; '
                       'xlat_tick(n, 2, 0); xlat_check(n, r_); } return;')
            return out
        if mn == 'BRK' or (mn == 'JMP' and mode == 'ind'):
            out.append('xlat_bail_to(n, %s); return;' % h4(pc))
            return out

        # ---- без операнда в памяти
        if mode == 'imp' or mode == 'acc':
            if mn in FLAG_OPS:
                out.append('%s XCYC(2, 0, %s);' % (FLAG_OPS[mn], h4(nxt)))
            elif mn in IFLAG_OPS:      # флаг I меняется после тактов (как у 6502)
                out.append('n->cpu.pc = %s; xlat_tick(n, 2, 0); %s xlat_check(n, %s);' % (h4(nxt), IFLAG_OPS[mn], h4(nxt)))
            elif mn in REG_OPS:
                out.append('%s XCYC(2, 0, %s);' % (REG_OPS[mn], h4(nxt)))
            elif mode == 'acc' or mn in ACC_OPS and mode == 'acc':
                out.append('%s XCYC(2, 0, %s);' % (ACC_OPS[mn], h4(nxt)))
            elif mn == 'PHA':
                out.append('XPUSH(XA); XCYC(3, 0b100, %s);' % h4(nxt))
            elif mn == 'PHP':
                out.append('XPUSH(XP | 0x30); XCYC(3, 0b100, %s);' % h4(nxt))
            elif mn == 'PLA':
                out.append('XA = XPULL(); XZN(XA); XCYC(4, 0, %s);' % h4(nxt))
            elif mn == 'PLP':
                out.append('n->cpu.pc = %s; xlat_tick(n, 4, 0); XP = (uint8_t)((XPULL() & ~0x10) | 0x20); xlat_check(n, %s);'
                           % (h4(nxt), h4(nxt)))
            else:
                out.append('xlat_io(n, %s, %s);' % (h4(pc), h4(nxt)))
            out.append(goto(nxt))
            return out

        if mode == 'imm':
            m = '0x%02X' % v
            out.append('%s XCYC(2, 0, %s); %s' % (self.alu(mn, m), h4(nxt), goto(nxt)))
            return out

        # ---- с операндом в памяти
        pre, addr, base, risky = self.ea(mode, v)
        if mode in ('zp', 'abs') and io_static(v):
            out.append('xlat_io(n, %s, %s); %s' % (h4(pc), h4(nxt), goto(nxt)))
            return out
        if mode == 'abs' and v >= 0x8000 and mn in WRITE_OPS | RMW_OPS:
            pass                                       # запись в маппер — обычная запись через шину
        io_check = ''
        if risky:
            if base:
                io_check = ('if (XIO(a_) || XIO((uint16_t)((b_ & 0xFF00) | (a_ & 0xFF)))) { xlat_io(n, %s, %s); %s } '
                            % (h4(pc), h4(nxt), goto(nxt)))
            else:
                io_check = 'if (XIO(a_)) { xlat_io(n, %s, %s); %s } ' % (h4(pc), h4(nxt), goto(nxt))
        rd = ('XZP(%s)' % addr) if mode in ('zp', 'zpx', 'zpy') else 'XRD(%s)' % addr
        if mn in READ_OPS:
            cnt, wm = READ_CYC[mode]
            cyc = str(cnt) if mode not in ('absx', 'absy', 'indy') else '%d + XCROSS(b_, a_)' % cnt
            out.append('{ %s%s%s XCYC(%s, %d, %s); } %s' % (pre, io_check, self.alu(mn, rd), cyc, wm, h4(nxt), goto(nxt)))
            return out
        if mn in WRITE_OPS:
            cnt, wm = WRITE_CYC[mode]
            reg = {'STA': 'XA', 'STX': 'XX', 'STY': 'XY'}[mn]
            wr = ('XZP(%s) = %s;' % (addr, reg)) if mode in ('zp', 'zpx', 'zpy') else 'XWR(%s, %s);' % (addr, reg)
            out.append('{ %s%s%s XCYC(%d, %d, %s); } %s' % (pre, io_check, wr, cnt, wm, h4(nxt), goto(nxt)))
            return out
        if mn in RMW_OPS:
            cnt, wm = RMW_CYC[mode]
            f = {'ASL': 'xlat_asl(n, m_)', 'LSR': 'xlat_lsr(n, m_)', 'ROL': 'xlat_rol(n, m_)',
                 'ROR': 'xlat_ror(n, m_)', 'INC': '(uint8_t)(m_ + 1)', 'DEC': '(uint8_t)(m_ - 1)'}[mn]
            zn = ' XZN(m_);' if mn in ('INC', 'DEC') else ''
            if mode in ('zp', 'zpx'):
                body_ = 'uint8_t m_ = XZP(%s); m_ = %s;%s XZP(%s) = m_;' % (addr, f, zn, addr)
            else:
                # чтение и две записи (старое, новое) — как у 6502; запись в ПЗУ/маппер видна шине
                body_ = 'uint8_t m_ = XRD(%s); XWR(%s, m_); m_ = %s;%s XWR(%s, m_);' % (addr, addr, f, zn, addr)
            out.append('{ %s%s%s XCYC(%d, %d, %s); } %s' % (pre, io_check, body_, cnt, wm, h4(nxt), goto(nxt)))
            return out
        out.append('xlat_io(n, %s, %s); %s' % (h4(pc), h4(nxt), goto(nxt)))
        return out

    @staticmethod
    def alu(mn, m):
        return {
            'LDA': 'XA = %s; XZN(XA);', 'LDX': 'XX = %s; XZN(XX);', 'LDY': 'XY = %s; XZN(XY);',
            'ADC': 'xlat_adc(n, %s);', 'SBC': 'xlat_adc(n, (uint8_t)~%s);',
            'AND': 'XA &= %s; XZN(XA);', 'ORA': 'XA |= %s; XZN(XA);', 'EOR': 'XA ^= %s; XZN(XA);',
            'CMP': 'xlat_cmp(n, XA, %s);', 'CPX': 'xlat_cmp(n, XX, %s);', 'CPY': 'xlat_cmp(n, XY, %s);',
            'BIT': 'xlat_bit(n, %s);',
        }[mn] % m

    def function(self, bank, entry, name):
        body, calls = self.body(bank, entry)
        lines = ['/* %s: банк %s, вход $%04X */' % (name, bank if 0x8000 <= entry < 0xC000 else 'fix', entry),
                 'static void %s(Nes *n) {' % name]
        nbytes = 0
        if entry not in body or body[entry] is None:
            lines.append('    xlat_bail_to(n, %s); return;' % h4(entry))
        else:
            lines.append('    goto L_%04X;' % entry)       # вход может быть не в начале тела
        for pc in sorted(body):
            d = body[pc]
            if d is None:
                lines.append('L_%04X: xlat_bail_to(n, %s); return;' % (pc, h4(pc)))
                continue
            nbytes += d[2]
            code = self.instr(bank, pc, d, name, body)
            pure_bail = d[0] == 'JSR' and d[3] in self.nonproc
            # инструкцию, которую сразу отдаём эмулятору, трассирует он сам
            lines.append(('L_%04X: /* %s %s */' if pure_bail else 'L_%04X: XT(0x%04X); /* %s %s */')
                         % ((pc, d[0], d[1]) if pure_bail else (pc, pc, d[0], d[1])))
            for c in code:
                lines.append('    ' + c)
        if body:
            lines.append('    return;')
        lines.append('}')
        return '\n'.join(lines), calls, nbytes


def load_coverage(path):
    cov = set()
    for line in open(path):
        p = line.split()
        if len(p) == 3 and p[0] == 'C':
            cov.add((int(p[1]), int(p[2], 16)))
    return cov


def main():
    rom = Rom(sys.argv[1])
    cov = load_coverage(sys.argv[2])
    want = []
    for line in open(sys.argv[3]):
        line = line.split('#')[0].split()
        if len(line) >= 3:
            want.append((int(line[0]), int(line[1], 16), line[2], line[3] if len(line) > 3 else None))
    closure = '--closure' in sys.argv
    exclude = set()
    for a in sys.argv:
        if a.startswith('--exclude='):
            for item in a[len('--exclude='):].split(','):
                b, ad = item.split(':')
                exclude.add((int(b), int(ad, 16)))

    g = Gen(rom, cov)
    done, order, out_fns, total, skipped = {}, list(want), [], 0, []
    while order:
        bank, entry, kind, name = order.pop(0)
        key = (g.bank_of(entry, bank), entry)
        if key in done or key in exclude or entry < 0x8000:
            continue
        if not g.covered(bank, entry):
            continue
        name = name or ('x_%s_%04X' % ('b%d' % key[0] if 0x8000 <= entry < 0xC000 else 'fix', entry))
        if not g.is_procedure(key[0], entry, kind):
            g.nonproc.add(entry)
            skipped.append(name)
            continue
        _, calls = g.body(key[0], entry)
        done[key] = (name, kind, 0)
        if closure:
            for t in sorted(calls):
                if 0x8000 <= t < 0xC000:
                    # банк вызываемой процедуры: тот, в котором она исполнялась
                    for b in range(7):
                        if (b, t) in cov:
                            order.append((b, t, 'jsr', None))
                else:
                    order.append((7, t, 'jsr', None))

    # адреса банковых процедур, которые где-то оказались переходниками, тоже отдаём эмулятору
    for key in list(done):
        if key[1] in g.nonproc:
            skipped.append(done.pop(key)[0])
    for key, (name, kind, _) in list(done.items()):
        code, _, nbytes = g.function(key[0], key[1], name)
        done[key] = (name, kind, nbytes)
        out_fns.append(code)
        total += nbytes

    with open(sys.argv[4], 'w') as f:
        f.write('/* СГЕНЕРИРОВАНО tools/xlat.py из ROM — не редактировать и не класть в репозиторий. */\n')
        f.write('#include "xlat.h"\n\n')
        for (bank, entry), (name, kind, nb) in done.items():
            f.write('static void %s(Nes *n);\n' % name)
        f.write('\n' + '\n\n'.join(out_fns) + '\n\n')
        f.write('const XlatInfo xlat_table[] = {\n')
        for (bank, entry), (name, kind, nb) in done.items():
            f.write('    { %d, 0x%04X, %d, %d, %s, "%s" },\n'
                    % (bank if 0x8000 <= entry < 0xC000 else -1, entry, 1 if kind == 'irq' else 0, nb, name, name))
        f.write('};\nconst int xlat_count = %d;\n\n' % len(done))
        f.write('void xlat_register_all(Nes *n) {\n')
        for (bank, entry), (name, kind, nb) in done.items():
            f.write('    nes_set_hook(n, %d, 0x%04X, %s);\n' % (bank, entry, name))
        f.write('}\n')
    print('translated %d routines, %d bytes of code; left to emulator (stack tricks): %s'
          % (len(done), total, ', '.join(skipped) or '-'))


if __name__ == '__main__':
    main()
