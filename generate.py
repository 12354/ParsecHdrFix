"""Generate ABI-preserving x64 BCrypt forwarders from Windows, independent of Parsec.

Only reads PE files. No execution, signature overrides, downloads, or install writes.
"""
import json
import os
from pathlib import Path
import sys

from pe_exports import PE

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
system = PE(Path(os.environ['SystemRoot']) / 'System32/bcrypt.dll')
assert system.bits == 64
exports = system.exports()
defs = ['LIBRARY bcrypt', 'EXPORTS']
asm = ['option casemap:none', 'EXTERN shim_resolve:PROC', '.code']
entries = []
for index, exp in enumerate(exports):
    names = exp['names']
    ordinal = exp['ordinal']
    entries.append('{%s, %d}' % (json.dumps(names[0]) if names else 'nullptr', ordinal))
    if names:
        for name in names:
            defs.append(f'    {name}=shim_forward_{index} @{ordinal}')
    else:
        defs.append(f'    shim_forward_{index} @{ordinal} NONAME')
    # Preserve every parameter register, stack arguments, and ABI stack alignment.
    asm += [f'shim_forward_{index} PROC FRAME', 'sub rsp, 0A8h', '.allocstack 0A8h', '.endprolog',
            'mov [rsp+20h], rcx', 'mov [rsp+28h], rdx', 'mov [rsp+30h], r8', 'mov [rsp+38h], r9',
            'movdqu [rsp+40h], xmm0', 'movdqu [rsp+50h], xmm1',
            'movdqu [rsp+60h], xmm2', 'movdqu [rsp+70h], xmm3',
            f'mov ecx, {index}', 'call shim_resolve',
            'mov rcx, [rsp+20h]', 'mov rdx, [rsp+28h]', 'mov r8, [rsp+30h]', 'mov r9, [rsp+38h]',
            'movdqu xmm0, [rsp+40h]', 'movdqu xmm1, [rsp+50h]',
            'movdqu xmm2, [rsp+60h]', 'movdqu xmm3, [rsp+70h]',
            'add rsp, 0A8h', 'jmp rax', f'shim_forward_{index} ENDP']
asm += ['END']
defs += ['    HdrShimGetStats @1000']
header = f'''// Generated from the system BCrypt export table only.
struct SYSTEM_EXPORT {{ const char *name; unsigned short ordinal; }};
static const SYSTEM_EXPORT system_exports[] = {{ {', '.join(entries)} }};
'''
for name, value in [('bcrypt.def', '\n'.join(defs)), ('forwarders.asm', '\n'.join(asm)), ('system_exports.h', header)]:
    path = out / name
    if not path.exists() or path.read_text() != value:
        path.write_text(value, encoding='utf-8')
print(f'Generated {len(exports)} BCrypt forwarders; no application binary inspected')
