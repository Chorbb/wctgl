"""symbols.py — чтение карты имён symbols/tgl.sym."""
import re

def load(path='symbols/tgl.sym'):
    ram, code = [], {}
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip() if not line.lstrip().startswith('#') else ''
        if not line:
            continue
        p = line.split(None, 5)
        kind = p[0]
        if kind in ('ram', 'arr'):
            ram.append({'kind': kind, 'addr': int(p[1].lstrip('$'), 16), 'size': int(p[2]), 'name': p[3],
                        'status': p[4], 'desc': p[5] if len(p) > 5 else ''})
        elif kind in ('code', 'spot', 'data'):
            code[(int(p[1]), int(p[2], 16))] = {'kind': kind, 'name': p[3], 'status': p[4],
                                                'desc': p[5] if len(p) > 5 else ''}
    return ram, code


def ram_name(ram, addr):
    """имя для адреса: 'life', 'chips+1', 'obj_x[3]' (для массивов — индекс ячейки)"""
    for r in ram:
        if r['addr'] <= addr < r['addr'] + r['size']:
            off = addr - r['addr']
            if r['kind'] == 'arr':
                return '%s[%d]' % (r['name'], off) if off else r['name']
            return r['name'] + ('+%d' % off if off else '')
    return None
