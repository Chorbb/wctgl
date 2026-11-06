#!/usr/bin/env python3
"""disasm.py — дизассемблер 6502 для ROM The Guardian Legend (UNROM, 8 банков по 16 КБ).

    python3 tools/disasm.py <rom.nes> <банк> <адрес_hex> [кол-во_инструкций]

Банк 7 — фиксированный ($C000-$FFFF), банки 0-6 подключаются в $8000-$BFFF.
"""
import sys

# мнемоника, режим адресации
OPS = {}
def _def(table, mode):
    for op, mn in table.items():
        OPS[op] = (mn, mode)

_def({0x69:'ADC',0x29:'AND',0xC9:'CMP',0xE0:'CPX',0xC0:'CPY',0x49:'EOR',0xA9:'LDA',0xA2:'LDX',0xA0:'LDY',0x09:'ORA',0xE9:'SBC',0xEB:'SBC*'}, 'imm')
_def({0x65:'ADC',0x25:'AND',0x06:'ASL',0x24:'BIT',0xC5:'CMP',0xE4:'CPX',0xC4:'CPY',0xC6:'DEC',0x45:'EOR',0xE6:'INC',0xA5:'LDA',0xA6:'LDX',0xA4:'LDY',0x46:'LSR',0x05:'ORA',0x26:'ROL',0x66:'ROR',0xE5:'SBC',0x85:'STA',0x86:'STX',0x84:'STY',0xA7:'LAX*',0x87:'SAX*',0xC7:'DCP*',0xE7:'ISB*'}, 'zp')
_def({0x75:'ADC',0x35:'AND',0x16:'ASL',0xD5:'CMP',0xD6:'DEC',0x55:'EOR',0xF6:'INC',0xB5:'LDA',0xB4:'LDY',0x56:'LSR',0x15:'ORA',0x36:'ROL',0x76:'ROR',0xF5:'SBC',0x95:'STA',0x94:'STY'}, 'zpx')
_def({0xB6:'LDX',0x96:'STX'}, 'zpy')
_def({0x6D:'ADC',0x2D:'AND',0x0E:'ASL',0x2C:'BIT',0xCD:'CMP',0xEC:'CPX',0xCC:'CPY',0xCE:'DEC',0x4D:'EOR',0xEE:'INC',0x4C:'JMP',0x20:'JSR',0xAD:'LDA',0xAE:'LDX',0xAC:'LDY',0x4E:'LSR',0x0D:'ORA',0x2E:'ROL',0x6E:'ROR',0xED:'SBC',0x8D:'STA',0x8E:'STX',0x8C:'STY',0xAF:'LAX*',0x8F:'SAX*',0xCF:'DCP*',0xEF:'ISB*'}, 'abs')
_def({0x7D:'ADC',0x3D:'AND',0x1E:'ASL',0xDD:'CMP',0xDE:'DEC',0x5D:'EOR',0xFE:'INC',0xBD:'LDA',0xBC:'LDY',0x5E:'LSR',0x1D:'ORA',0x3E:'ROL',0x7E:'ROR',0xFD:'SBC',0x9D:'STA'}, 'absx')
_def({0x79:'ADC',0x39:'AND',0xD9:'CMP',0x59:'EOR',0xB9:'LDA',0xBE:'LDX',0x19:'ORA',0xF9:'SBC',0x99:'STA'}, 'absy')
_def({0x61:'ADC',0x21:'AND',0xC1:'CMP',0x41:'EOR',0xA1:'LDA',0x01:'ORA',0xE1:'SBC',0x81:'STA'}, 'indx')
_def({0x71:'ADC',0x31:'AND',0xD1:'CMP',0x51:'EOR',0xB1:'LDA',0x11:'ORA',0xF1:'SBC',0x91:'STA'}, 'indy')
_def({0x6C:'JMP'}, 'ind')
_def({0x10:'BPL',0x30:'BMI',0x50:'BVC',0x70:'BVS',0x90:'BCC',0xB0:'BCS',0xD0:'BNE',0xF0:'BEQ'}, 'rel')
_def({0x0A:'ASL',0x4A:'LSR',0x2A:'ROL',0x6A:'ROR'}, 'acc')
_def({0x00:'BRK',0x18:'CLC',0xD8:'CLD',0x58:'CLI',0xB8:'CLV',0xCA:'DEX',0x88:'DEY',0xE8:'INX',0xC8:'INY',0xEA:'NOP',
      0x48:'PHA',0x08:'PHP',0x68:'PLA',0x28:'PLP',0x40:'RTI',0x60:'RTS',0x38:'SEC',0xF8:'SED',0x78:'SEI',
      0xAA:'TAX',0xA8:'TAY',0xBA:'TSX',0x8A:'TXA',0x9A:'TXS',0x98:'TYA'}, 'imp')
SIZE = {'imm':2,'zp':2,'zpx':2,'zpy':2,'abs':3,'absx':3,'absy':3,'indx':2,'indy':2,'ind':3,'rel':2,'acc':1,'imp':1}

# известные адреса железа NES
HW = {0x2000:'PPUCTRL',0x2001:'PPUMASK',0x2002:'PPUSTATUS',0x2003:'OAMADDR',0x2004:'OAMDATA',
      0x2005:'PPUSCROLL',0x2006:'PPUADDR',0x2007:'PPUDATA',0x4014:'OAMDMA',0x4015:'APUSTATUS',
      0x4016:'JOY1',0x4017:'JOY2'}


class Rom:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.prg = d[16:16 + d[4] * 0x4000]
        self.banks = d[4]

    def byte(self, bank, addr):
        if addr >= 0xC000:
            return self.prg[(self.banks - 1) * 0x4000 + addr - 0xC000]
        return self.prg[bank * 0x4000 + addr - 0x8000]


def fmt_operand(mode, v, pc):
    name = HW.get(v, '')
    tag = ('  ; ' + name) if name else ''
    if mode == 'imm':  return '#$%02X' % v, ''
    if mode == 'zp':   return '$%02X' % v, ''
    if mode == 'zpx':  return '$%02X,X' % v, ''
    if mode == 'zpy':  return '$%02X,Y' % v, ''
    if mode == 'abs':  return '$%04X' % v, tag
    if mode == 'absx': return '$%04X,X' % v, tag
    if mode == 'absy': return '$%04X,Y' % v, tag
    if mode == 'indx': return '($%02X,X)' % v, ''
    if mode == 'indy': return '($%02X),Y' % v, ''
    if mode == 'ind':  return '($%04X)' % v, ''
    if mode == 'rel':  return '$%04X' % ((pc + 2 + (v - 256 if v > 127 else v)) & 0xFFFF), ''
    if mode == 'acc':  return 'A', ''
    return '', ''


def disasm(rom, bank, pc, count):
    out = []
    for _ in range(count):
        op = rom.byte(bank, pc)
        mn, mode = OPS.get(op, ('.db $%02X' % op, 'imp'))
        size = SIZE[mode] if op in OPS else 1
        raw = [rom.byte(bank, (pc + i) & 0xFFFF) for i in range(size)]
        v = raw[1] if size == 2 else (raw[1] | raw[2] << 8) if size == 3 else 0
        arg, tag = fmt_operand(mode, v, pc)
        out.append('%04X  %-9s %-5s %s%s' % (pc, ' '.join('%02X' % b for b in raw), mn, arg, tag))
        pc = (pc + size) & 0xFFFF
    return out


if __name__ == '__main__':
    rom = Rom(sys.argv[1])
    bank = int(sys.argv[2])
    addr = int(sys.argv[3], 16)
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    print('\n'.join(disasm(rom, bank, addr, n)))
