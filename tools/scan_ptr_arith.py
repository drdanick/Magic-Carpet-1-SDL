#!/usr/bin/env python3
"""Find Hex-Rays "byte offset added to a typed pointer" bugs in sub_main.cpp.

IDA emitted e.g. `*(_BYTE *)(ptr + 392)` with `ptr` as a 32-bit int. When `ptr` later became a typed
pointer (Type_160*, TColor*, ...) the expression silently started scaling by sizeof(*ptr).
This lists dereferences `*(<cast> *)(... NAME + ...)` / `(NAME + n)` where NAME is a pointer
variable/field declared with a non-byte pointee type. Review each hit by hand.
"""
import re, sys
srcs = ['sub_main.cpp', 'sub_main.h', 'engine/Basic.h']
decl = re.compile(r'^\s*(?:extern\s+|static\s+)?(?:struct\s+)?(\w+)\s*\*\s*(\w+)\s*(?:=[^;]*)?;', re.M)
kw = {'else', 'return', 'result', 'case', 'goto', 'do'}
byte_types = {'uint8_t', 'char', 'uint8', 'int8_t', 'int8', '_BYTE', 'void', 'unsigned', 'FILE', 'const'}
names = {}
for f in srcs:
    for t, n in decl.findall(open(f, encoding='latin-1').read()):
        if t not in byte_types and t not in kw and n not in kw and not n.startswith('v') and len(n) > 3:
            names[n] = t
lines = open('sub_main.cpp', encoding='latin-1').read().split('\n')
pat = re.compile(r'\*\s*\(\s*(?:unsigned\s+)?(?:_BYTE|_WORD|_DWORD|__int8|__int16|int|uint8_t|uint16_t|uint32_t|int16_t|int32_t|char|__int16|int16|uint16|uint32|uint8)\s*\*\s*\)\s*\(([^;]*)')
count = 0
for i, l in enumerate(lines, 1):
    if l.lstrip().startswith('//'):
        continue
    for m in pat.finditer(l):
        inner = m.group(1)
        for n, t in names.items():
            if re.search(r'(?<![\w>])' + n + r'\s*\+|\+\s*' + n + r'\b(?!\s*\[)', inner) and \
               not re.search(r'\(\s*(?:uint8_t|char|uint8|_BYTE|int|uint32|uint32_t)\s*\*?\s*\)\s*' + n + r'\b', inner):
                print(f'{i}: [{n}: {t}*] {l.strip()[:140]}')
                count += 1
print(f'{count} suspicious lines', file=sys.stderr)
