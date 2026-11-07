#!/usr/bin/env python3
"""wavecheck.py — проверка расшифровки сценариев волн по записи.

    python3 tools/wavecheck.py rom.nes build/scripts_ev.txt

scripts_ev.txt — от `XT_SCRIPT=1 xtest statwatch`: R (новый отрезок), P (начало сценария),
E (появился враг), G (появился генератор), F (не нашлось места). Для каждого сценария
сравнивается, что игра выпустила, с тем, что предсказывает tools/waves.py.
"""
import sys, os
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import waves
waves.init(sys.argv[1])
g = {'decode': waves.decode}
runs, cur = [], None
for l in open(sys.argv[2]):
    p = l.split()
    if p[0] == 'R': cur = None
    elif p[0] == 'P': cur = {'bank': int(p[2]), 'ptr': int(p[1], 16), 'ev': []}; runs.append(cur)
    elif cur is not None and p[0] in ('E', 'G', 'F'): cur['ev'].append((p[0], int(p[1])))
ok = bad = events = 0
for r in runs:
    exp = [('E', v) if k == 'враг' else ('G', v[0]) for k, v, *_ in g['decode'](r['bank'], r['ptr']) if k in ('враг', 'генератор')]
    got = [e for e in r['ev'] if e[0] != 'F']
    n = min(len(got), len(exp))
    if n == 0: continue
    events += n
    if got[:n] == exp[:n]: ok += 1
    else:
        bad += 1
        print('расхождение: банк %d $%04X  ожидалось %s  было %s' % (r['bank'], r['ptr'], exp[:6], got[:6]))
print('сценариев сверено %d: совпало %d, расхождений %d; появлений сверено %d' % (ok + bad, ok, bad, events))
