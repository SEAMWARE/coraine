#!/usr/bin/env python3
#
# FILE            sanReport.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# sanReport.py <report dir> [frames] - the reports of a sanitizer run (test/sanitizer/sanSetup.sh),
# deduplicated by kind and first frames, each with the tests that hit it. Markdown on stdout.
#
# Exit code: 0 if the directory holds no report, 1 otherwise.
#
import collections
import os
import re
import sys

R = sys.argv[1]
NF = int(sys.argv[2]) if len(sys.argv) > 2 else 5

#
# pid -> the test (section) that started the process, from the wrappers' pids file
#
pid2test = {}
if os.path.exists(os.path.join(R, 'pids')):
    for line in open(os.path.join(R, 'pids'), errors='replace'):
        parts = line.split()
        if not parts:
            continue
        m = re.search(r'cases/([^ /]+?)\.(init|run|teardown|test)\b', line)
        if m:
            pid2test[parts[0]] = m.group(1)
        elif line.rstrip().endswith('--version'):
            pid2test[parts[0]] = '(--version)'
        else:
            m = re.search(r'\[([^]]*)\]', line)
            pid2test[parts[0]] = (m.group(1) if m else '?')[:60]


def where(loc):
    return re.sub(r'^.*/(stack|san|git)/', '', loc)


def frames(block):
    out = []
    for line in block:
        m = re.match(r'\s+#\d+ 0x[0-9a-f]+ in (\S+) (\S+)', line)
        if m:
            fn, loc = m.group(1), m.group(2)
            if 'libsanitizer' in loc or fn.startswith('__interceptor') or fn in (
                    'malloc', 'calloc', 'realloc', 'free', 'strdup', 'strndup', '__asan_memcpy', '__asan_memset'):
                continue
            out.append(f'{fn} {where(loc)}')
        else:
            m = re.match(r'\s+#\d+ 0x[0-9a-f]+ +\((\S+)\+', line)
            if m:
                out.append(os.path.basename(m.group(1)))
    return out


groups = collections.OrderedDict()
files = 0
for f in sorted(os.listdir(R)):
    if f == 'pids' or not re.match(r'^(asan|ubsan|stderr)\.', f):
        continue
    files += 1
    pid = f.split('.')[-1]
    test = pid2test.get(pid, '?pid' + pid)
    lines = open(os.path.join(R, f), errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        kind = None
        if 'runtime error:' in line:
            # the operands' values out of the message: one finding, whatever the values were
            what = re.sub(r'0x[0-9a-f]+', 'ADDR', line.split('runtime error:')[1].strip())
            what = re.sub(r'(?<![\w.])-?\d+(?![\w.])', 'N', what)
            kind = 'UBSAN ' + where(line.split(': runtime error')[0]) + ' ' + what
        elif re.match(r'^(Direct|Indirect) leak of', line):
            kind = 'LEAK ' + line.split(' leak')[0]
        elif 'ERROR: AddressSanitizer:' in line:
            kind = 'ASAN ' + re.sub(r' on .*', '', line.split('ERROR: AddressSanitizer: ')[1])
        elif 'ERROR: LeakSanitizer:' in line and 'detected memory leaks' not in line:
            kind = 'LSAN ' + line.split('ERROR: LeakSanitizer: ')[1]
        if kind is None:
            i += 1
            continue
        j = i + 1
        while j < len(lines) and lines[j].strip() and not re.match(r'^(Direct|Indirect) leak', lines[j]) \
                and 'runtime error' not in lines[j]:
            j += 1
        fr = frames(lines[i + 1:j])
        key = (kind, tuple(fr[:NF]))
        g = groups.setdefault(key, {'tests': collections.Counter(), 'n': 0})
        g['tests'][test] += 1
        g['n'] += 1
        i = j

if not groups:
    print(f'No sanitizer report ({files} report files).')
    sys.exit(0)

print(f'**{len(groups)} distinct sanitizer finding(s)**, {sum(g["n"] for g in groups.values())} reports in {files} files')
print()
for (kind, fr), g in sorted(groups.items(), key=lambda kv: kv[0][0]):
    print('<details><summary><code>' + kind.replace('<', '&lt;') + f'</code> - {g["n"]} report(s), {len(g["tests"])} test(s)</summary>')
    print()
    print('```')
    for x in fr:
        print('   ', x)
    print('tests:', ', '.join(f'{t}({n})' for t, n in g['tests'].most_common(12)) + (' ...' if len(g['tests']) > 12 else ''))
    print('```')
    print('</details>')
sys.exit(1)
