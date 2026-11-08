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
import os  # noqa: E402
import symbols as symtab  # noqa: E402

SYM_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'symbols', 'tgl.sym')
SYM_RAM, SYM_CODE = symtab.load(SYM_PATH) if os.path.exists(SYM_PATH) else ([], {})


def operand_note(mn, mode, v):
    """имя переменной для комментария: 'life', 'obj_x,X'"""
    if mode in ('zp', 'abs', 'zpx', 'zpy', 'absx', 'absy', 'indx', 'indy'):
        nm = symtab.ram_name(SYM_RAM, v)
        if nm:
            suf = {'zpx': ',X', 'absx': ',X', 'zpy': ',Y', 'absy': ',Y', 'indx': ' (,X)', 'indy': ' (),Y'}.get(mode, '')
            return ' ' + nm + suf
    return ''

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
        self.hooks = set()             # точки для модов: (банк, адрес) → XHOOK
        self.nonproc = set()           # адреса переходников (не обычных процедур)
        self.loop_entry = None
        self.cur_entry = None
        self.loop_heads = set()
        self._disp = {}
        self.far = set()               # переходники дальнего вызова: JSR far / .byte банк, адрес-1 (2 байта)

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
            if pc in self.loop_heads and pc != entry:
                continue                     # начало главного цикла — граница любой процедуры
            d = self.decode(bank, pc)
            seen[pc] = d
            if d is None:
                continue
            mn, mode, size, v = d
            nxt = (pc + size) & 0xFFFF
            if mn in BRANCHES:
                todo += [nxt, (nxt + (v - 256 if v > 127 else v)) & 0xFFFF]
            elif mn == 'JSR' and v in self.far:
                calls.add(('far',) + self.far_target(bank, pc))
                todo.append((pc + 6) & 0xFFFF)          # после трёх байт параметров
            elif mn == 'JSR':
                calls.add(v)
                todo.append(nxt)
            elif mn == 'JMP' and mode == 'abs':
                todo.append(v)
            elif mn == 'RTS' and self.dispatch_ret(bank, pc) is not None:
                todo.append(self.dispatch_ret(bank, pc))
            elif mn in ('RTS', 'RTI', 'BRK') or (mn == 'JMP' and mode == 'ind'):
                pass
            else:
                todo.append(nxt)
            if len(seen) > 3000:
                break
        return seen, calls

    def dispatch_ret(self, bank, pc):
        """RTS как вызов по таблице: перед ним в стек кладут адрес возврата (LDA #hi PHA LDA #lo PHA),
        затем адрес обработчика из таблицы (LDA tab,Y PHA LDA tab,Y PHA). Возвращает адрес,
        куда вернётся обработчик, или None."""
        key = (bank, pc)
        if key in self._disp:
            return self._disp[key]
        res = None
        for k in range(8, 24):
            a, seq = pc - k, []
            while a < pc and len(seq) < 16:
                d = self.decode(bank, a)
                if d is None:
                    break
                seq.append(d)
                a += d[2]
            if a != pc or len(seq) < 8:
                continue
            t = seq[-8:]
            mn = [x[0] for x in t]
            if mn == ['LDA', 'PHA', 'LDA', 'PHA', 'LDA', 'PHA', 'LDA', 'PHA'] and t[0][1] == 'imm' and t[2][1] == 'imm' \
                    and t[4][1] != 'imm' and t[6][1] != 'imm':
                res = ((t[0][3] << 8) | t[2][3]) + 1 & 0xFFFF
                break
        self._disp[key] = res
        return res

    def far_target(self, bank, pc):
        """Параметры дальнего вызова после JSR: банк, затем адрес входа минус 1 (переходник
        переходит туда через RTS)."""
        b = self.rom.byte(bank, (pc + 3) & 0xFFFF) & 7
        t = (self.rom.byte(bank, (pc + 4) & 0xFFFF) | self.rom.byte(bank, (pc + 5) & 0xFFFF) << 8) + 1
        return b, t & 0xFFFF

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
            if mn == 'RTS' and self.dispatch_ret(bank, pc) is not None:
                if dep < 4:
                    return False
                todo.append((self.dispatch_ret(bank, pc), dep - 4))
                continue
            if mn in ('RTS', 'RTI'):
                if mn != end or dep != 0:
                    return False
                continue
            if mn in BRANCHES:
                todo += [(nxt, dep), ((nxt + (v - 256 if v > 127 else v)) & 0xFFFF, dep)]
            elif mn == 'JSR' and v in self.far:
                todo.append(((pc + 6) & 0xFFFF, dep))
            elif mn == 'JMP' and mode == 'abs':
                if kind == 'loop' and v == entry:
                    if dep != 0:
                        return False
                    continue
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
            if (self.loop_entry is not None and addr == self.loop_entry) or \
                    (addr in self.loop_heads and addr != self.cur_entry):
                return 'return;'        # круг цикла закончен: следующий круг начнётся с nes_step
            if addr in body and body[addr] is not None:
                if addr <= pc:              # переход назад: цикл — не держать управление слишком долго
                    return 'if (xlat_stop(n)) return; goto L_%04X;' % addr
                return 'goto L_%04X;' % addr
            return 'xlat_bail_to(n, %s); return;' % h4(addr)

        def io_static(addr):
            return 0x2000 <= addr < 0x4020

        if mn.endswith('*'):                         # неофициальные — эмулятором
            out.append('XIOX(%s, %s); %s' % (h4(pc), h4(nxt), goto(nxt)))
            return out

        # ---- управление
        if mn in BRANCHES:
            mask, setv = BRANCHES[mn]
            to = (nxt + (v - 256 if v > 127 else v)) & 0xFFFF
            cond = '(XP & %s)' % mask if setv else '!(XP & %s)' % mask
            out.append('if (%s) { XBR(1, %s, %s); %s }' % (cond, h4(nxt), h4(to), goto(to)))
            out.append('XBR(0, %s, %s); %s' % (h4(nxt), h4(to), goto(nxt)))
            return out
        if mn == 'JMP' and mode == 'abs':
            out.append('XCYC(3, 0, %s);' % h4(v))
            if (self.loop_entry is not None and v == self.loop_entry) or \
                    (v in self.loop_heads and v != self.cur_entry):
                out.append('return;')
            elif v in body and body[v] is not None:
                out.append(('if (xlat_stop(n)) return; ' if v <= pc else '') + 'goto L_%04X;' % v)
            else:
                out.append('xlat_jump(n, %s); return;' % h4(v))
            return out
        if mn == 'JSR' and v in self.far:
            # дальний вызов: переходник исполняет эмулятор, вызываемая процедура в другом
            # банке подхватывается своим переводом (если он есть) по адресу входа
            ret = (pc + 2) & 0xFFFF
            after = (pc + 6) & 0xFFFF
            out.append('XPUSH(%s); XPUSH(%s); XCYC(6, 0b11000, %s);' % (h4(ret >> 8), h4(ret & 0xFF), h4(v)))
            out.append('if (!xlat_far(n, %s)) return; %s' % (h4(after), goto(after)))
            return out
        if mn == 'JSR' and v in self.nonproc:
            out.append('xlat_bail_to(n, %s); return;' % h4(pc))
            return out
        if mn == 'JSR':
            ret = (pc + 2) & 0xFFFF
            out.append('XPUSH(%s); XPUSH(%s); XCYC(6, 0b11000, %s);' % (h4(ret >> 8), h4(ret & 0xFF), h4(v)))
            out.append('if (!xlat_call(n, %s, %s)) return; %s' % (h4(v), h4(nxt), goto(nxt)))
            return out
        if mn == 'RTS' and self.dispatch_ret(bank, pc) is not None:
            ret = self.dispatch_ret(bank, pc)
            # обработчик вернётся в ret: адрес возврата уже в стеке, как после JSR
            out.append('{ uint8_t lo_ = XPULL(); uint8_t hi_ = XPULL(); uint16_t r_ = (uint16_t)((lo_ | (hi_ << 8)) + 1);'
                       ' XCYC(6, 0, r_); if (!xlat_call(n, r_, %s)) return; } %s' % (h4(ret), goto(ret)))
            return out
        if mn == 'RTS':
            out.append('{ uint8_t lo_ = XPULL(); uint8_t hi_ = XPULL(); uint16_t r_ = (uint16_t)((lo_ | (hi_ << 8)) + 1);'
                       ' XCYC(6, 0, r_); } return;')
            return out
        if mn == 'RTI':
            out.append('{ xlat_tick(n, 4, 0); XP = (uint8_t)((XPULL() & ~0x10) | 0x20); uint8_t lo_ = XPULL(); '
                       'uint8_t hi_ = XPULL(); uint16_t r_ = (uint16_t)(lo_ | (hi_ << 8)); n->cpu.pc = r_; '
                       'xlat_tick(n, 2, 0); XCHK(r_); } return;')
            return out
        if mn == 'BRK' or (mn == 'JMP' and mode == 'ind'):
            out.append('xlat_bail_to(n, %s); return;' % h4(pc))
            return out

        # ---- без операнда в памяти
        if mode == 'imp' or mode == 'acc':
            if mn in FLAG_OPS:
                out.append('%s XCYC(2, 0, %s);' % (FLAG_OPS[mn], h4(nxt)))
            elif mn in IFLAG_OPS:      # флаг I меняется после тактов (как у 6502)
                out.append('n->cpu.pc = %s; xlat_tick(n, 2, 0); %s XCHK(%s);' % (h4(nxt), IFLAG_OPS[mn], h4(nxt)))
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
                out.append('n->cpu.pc = %s; xlat_tick(n, 4, 0); XP = (uint8_t)((XPULL() & ~0x10) | 0x20); XCHK(%s);'
                           % (h4(nxt), h4(nxt)))
            else:
                out.append('XIOX(%s, %s);' % (h4(pc), h4(nxt)))
            out.append(goto(nxt))
            return out

        if mode == 'imm':
            m = '0x%02X' % v
            out.append('%s XCYC(2, 0, %s); %s' % (self.alu(mn, m), h4(nxt), goto(nxt)))
            return out

        # ---- с операндом в памяти
        pre, addr, base, risky = self.ea(mode, v)
        if mode in ('zp', 'abs') and io_static(v):
            out.append('XIOX(%s, %s); %s' % (h4(pc), h4(nxt), goto(nxt)))
            return out
        if mode == 'abs' and v >= 0x8000 and mn in WRITE_OPS | RMW_OPS:
            pass                                       # запись в маппер — обычная запись через шину
        io_check = ''
        if risky:
            if base:
                io_check = ('if (XIO(a_) || XIO((uint16_t)((b_ & 0xFF00) | (a_ & 0xFF)))) { XIOX(%s, %s); %s } '
                            % (h4(pc), h4(nxt), goto(nxt)))
            else:
                io_check = 'if (XIO(a_)) { XIOX(%s, %s); %s } ' % (h4(pc), h4(nxt), goto(nxt))
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
        out.append('XIOX(%s, %s); %s' % (h4(pc), h4(nxt), goto(nxt)))
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

    def function(self, bank, entry, name, kind='jsr'):
        self.loop_entry = entry if kind == 'loop' else None
        self.cur_entry = entry
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
            note = operand_note(d[0], d[1], d[3])
            lines.append(('L_%04X: /* %s %s%s */' if pure_bail else 'L_%04X: XT(0x%04X); /* %s %s%s */')
                         % ((pc, d[0], d[1], note) if pure_bail else (pc, pc, d[0], d[1], note)))
            if (self.bank_of(pc, bank), pc) in self.hooks:
                lines.append('    XHOOK(%s);' % h4(pc))
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


def load_rts_targets(path):
    t = set()
    for line in open(path):
        p = line.split()
        if len(p) == 3 and p[0] == 'T':
            t.add((int(p[1]), int(p[2], 16)))
    return t


def main():
    rom = Rom(sys.argv[1])
    cov = load_coverage(sys.argv[2])
    want, far_entries, hooks = [], [], set()
    for line in open(sys.argv[3]):
        line = line.split('#')[0].split()
        if len(line) == 2 and line[0] == 'far':
            far_entries.append(int(line[1], 16))
        elif len(line) == 3 and line[0] == 'hook':
            hooks.add((int(line[1]), int(line[2], 16)))
        elif len(line) >= 3:
            want.append((int(line[0]), int(line[1], 16), line[2], line[3] if len(line) > 3 else None))
    closure = '--closure' in sys.argv
    exclude = set()
    for a in sys.argv:
        if a.startswith('--exclude='):
            for item in a[len('--exclude='):].split(','):
                b, ad = item.split(':')
                exclude.add((int(b), int(ad, 16)))

    g = Gen(rom, cov)
    g.hooks = hooks
    g.far = set(far_entries)
    g.loop_heads = {e for b, e, k, nm in want if k == 'loop'}
    # входы, куда попадали через RTS не как возврат из JSR: обработчики из таблиц переходов
    # и продолжения после переходников. Переводим их как отдельные процедуры.
    if '--rts-entries' in sys.argv:
        for b, t in sorted(load_rts_targets(sys.argv[2])):
            bb = b if 0x8000 <= t < 0xC000 else 7
            def byte(a):
                return rom.byte(bb, a & 0xFFFF) if (a >= 0xC000 or 0x8000 <= a < 0xC000) else None
            jsr3 = byte(t - 3) == 0x20
            far6 = byte(t - 6) == 0x20 and ((byte(t - 5) or 0) | (byte(t - 4) or 0) << 8) in g.far
            if not jsr3 and not far6:
                want.append((bb, t, 'jsr', None))
    done, order, out_fns, total, skipped = {}, list(want), [], 0, []
    while order:
        bank, entry, kind, name = order.pop(0)
        key = (g.bank_of(entry, bank), entry)
        if key in done or key in exclude or entry < 0x8000:
            continue
        if not g.covered(bank, entry):
            continue
        sym = SYM_CODE.get((key[0] if 0x8000 <= entry < 0xC000 else 7, entry))
        if not name and sym and sym['kind'] == 'code':
            name = sym['name'] + ('_b%d' % key[0] if 0x8000 <= entry < 0xC000 else '')
        name = name or ('x_%s_%04X' % ('b%d' % key[0] if 0x8000 <= entry < 0xC000 else 'fix', entry))
        if not g.is_procedure(key[0], entry, kind):
            g.nonproc.add(entry)
            skipped.append(name)
            continue
        body_, calls = g.body(key[0], entry)
        first = body_.get(entry)
        if first is None or (first[0] == 'JSR' and first[3] in g.nonproc) or first[0] == 'BRK' \
                or (first[0] == 'JMP' and first[1] == 'ind'):
            skipped.append(name)            # сразу отдал бы управление: переводить нечего
            continue
        done[key] = (name, kind, 0)
        if closure:
            for t in sorted(calls, key=lambda c: (1, c[1], c[2]) if isinstance(c, tuple) else (0, c, 0)):
                if isinstance(t, tuple):            # дальний вызов: банк известен из параметров
                    order.append((t[1], t[2], 'jsr', None))
                elif 0x8000 <= t < 0xC000:
                    # банк вызываемой процедуры: тот, в котором она исполнялась
                    for b in range(7):
                        if (b, t) in cov:
                            order.append((b, t, 'jsr', None))
                else:
                    order.append((7, t, 'jsr', None))

    uniq = set()
    # адреса банковых процедур, которые где-то оказались переходниками, тоже отдаём эмулятору
    for key in list(done):
        if key[1] in g.nonproc:
            skipped.append(done.pop(key)[0])
    for key, (name, kind, _) in list(done.items()):
        code, _, nbytes = g.function(key[0], key[1], name, kind)
        for pc_, d_ in g.body(key[0], key[1])[0].items():
            if d_ is not None:
                uniq.add((g.bank_of(pc_, key[0]), pc_, d_[2]))
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
                    % (bank if 0x8000 <= entry < 0xC000 else -1, entry, {'jsr': 0, 'irq': 1, 'loop': 2}[kind], nb, name, name))
        f.write('};\nconst int xlat_count = %d;\n\n' % len(done))
        f.write('void xlat_register_all(Nes *n) {\n')
        for (bank, entry), (name, kind, nb) in done.items():
            f.write('    nes_set_hook(n, %d, 0x%04X, %s);\n' % (bank, entry, name))
        f.write('}\n')
    print('unique translated code: %d bytes' % sum(u[2] for u in uniq))
    print('translated %d routines, %d bytes of code; left to emulator (stack tricks): %s'
          % (len(done), total, ', '.join(sorted(set(skipped))) or '-'))


if __name__ == '__main__':
    main()
