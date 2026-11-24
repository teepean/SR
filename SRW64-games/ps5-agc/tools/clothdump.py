#! /usr/bin/env python3
# clothdump.py <dump file> - prints a libysr YSR_CLOTHDUMP snapshot: the work items (phase << 61 | record), per record
# the pipeline, its shader header (thread group size at +0x40) and the 304-byte user data table, as qwords; qwords which
# are the same in all tables are marked "same", pointers which differ per record are candidates for per-instance buffers
import collections, struct, sys

data = open(sys.argv[1], 'rb').read()
records, items, global_table = struct.unpack_from('<iiQ', data, 0)
global_bytes = data[16:80]
item = struct.unpack_from('<512Q', data, 80)[:items]
RECORD = 16 + 64 + 96 + 304
recs = []
for i in range(records):
    o = 80 + 512 * 8 + i * RECORD
    pipeline, table = struct.unpack_from('<QQ', data, o)
    header = data[o + 80:o + 176]
    recs.append((pipeline, table, data[o + 16:o + 80], header, data[o + 176:o + 176 + 304]))
print('records %d items %d global table 0x%x' % (records, items, global_table))
print('global table qwords:', ' '.join('%016x' % q for q in struct.unpack_from('<8Q', global_bytes)))
phases = collections.Counter(q >> 61 for q in item)
print('items per phase:', dict(sorted(phases.items())))
print('item order (phase:record):', ' '.join('%d:%d' % (q >> 61, q & 0xfffff) for q in item[:80]), '...' if items > 80 else '')
pipelines = collections.Counter(r[0] for r in recs)
for p, n in pipelines.most_common():
    h = [r[3] for r in recs if r[0] == p][0]
    tg = struct.unpack_from('<3I', h, 0x40)
    print('pipeline 0x%x: %d records, thread group %s' % (p, n, tg))
tables = [struct.unpack_from('<38Q', r[4]) for r in recs]
if tables:
    print('table qwords (offset: value in record 0 | same/differs, distinct values):')
    for k in range(38):
        values = [t[k] for t in tables]
        distinct = len(set(values))
        print('  +0x%03x %016x  %s %d' % (8 * k, values[0], 'same   ' if distinct == 1 else 'differs', distinct))
    print('per record: table address, pipeline, qwords +0x10..+0x30:')
    for i, r in enumerate(recs[:40]):
        print('  %3d table 0x%x pipeline 0x%x  %s' % (i, r[1], r[0], ' '.join('%016x' % q for q in tables[i][2:7])))
