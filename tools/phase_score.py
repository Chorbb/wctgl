#!/usr/bin/env python3
"""phase_score.py — насколько точно процессор порта совпадает по тактам с Mesen.

Читает строки `R ...` из `replay` (TGL_REGS) со стандартного ввода. Для каждого кадра
предсказывает, какой PC показал бы Mesen, если бы его процессор был там же, где наш
(Mesen останавливается посреди инструкции: PC увеличивается на каждом такте выборки байта),
и сравнивает с PC из записи. Печатает долю совпадений и распределение сдвигов.

    TGL_REGS=1 TGL_REGS_TO=1000 TGL_NOSYNC=1 build/replay rom log 1000 | python3 tools/phase_score.py rom
"""
import re
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from disasm import Rom, OPS, SIZE  # noqa: E402

rom = Rom(sys.argv[1])
LINE = re.compile(r'R\s+(\d+) (.) port inside ([0-9A-F]{4}) \+(-?\d+) cyc \| orig pc=([0-9A-F]{4})')


def size_at(addr):
    op = rom.byte(0, addr) if addr >= 0xC000 else None
    if op is None:
        return None          # банк неизвестен — пропускаем
    return SIZE[OPS[op][1]] if op in OPS else 1


def predicted(pc, k):
    """PC, который показал бы Mesen после k тактов внутри инструкции по адресу pc."""
    n = size_at(pc)
    if n is None:
        return None
    return pc + min(k, n)


total = hit = 0
shift = Counter()
for line in sys.stdin:
    m = LINE.match(line)
    if not m:
        continue
    pc, k, orig = int(m.group(3), 16), int(m.group(4)), int(m.group(5), 16)
    p = predicted(pc, k)
    if p is None or k > 10:
        continue
    total += 1
    if p == orig:
        hit += 1
    elif '-v' in sys.argv:
        print('  frame %s: port %04X+%d -> %04X, orig %04X (%+d)' % (m.group(1), pc, k, p, orig, orig - p))
    shift[orig - p] += 1

print('frames scored: %d, exact phase: %d (%.1f%%)' % (total, hit, 100.0 * hit / max(total, 1)))
print('orig_pc - predicted_pc:', ', '.join('%+d:%d' % kv for kv in sorted(shift.items())))
