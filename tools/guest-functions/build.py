#!/usr/bin/env python3
"""Build shadPS4 x86-64 guest function packages; no Android/host code in a package."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import importlib.util

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / 'guest/custom/v1'
SDK_IMPORTS = ('shad_sdk_query', 'shad_sdk_clock_ns', 'shad_sdk_counter', 'shad_sdk_log')

def sha(data):
    return hashlib.sha256(data).hexdigest()

def unique(pairs):
    result = {}
    for k, v in pairs:
        if k in result:
            raise ValueError('duplicate JSON key: ' + k)
        result[k] = v
    return result

def identifier(s):
    if not isinstance(s, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,95}', s):
        raise ValueError('invalid symbol: ' + str(s))
    return s

def read_elf(path):
    b = path.read_bytes()
    if len(b) < 64 or b[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected ELF64 LE')
    h = struct.unpack_from('<16sHHIQQQIHHHHHH', b)
    if h[2] != 62 or h[11] != 64 or not h[12] or h[13] >= h[12]:
        raise ValueError('expected x86-64 ELF with sections')
    sections = [struct.unpack_from('<IIQQQQIIQQ', b, h[6]+i*64) for i in range(h[12])]
    def take(offset, size):
        if offset < 0 or size < 0 or offset+size > len(b):
            raise ValueError('truncated ELF')
        return b[offset:offset+size]
    names = take(sections[h[13]][4], sections[h[13]][5])
    def string(table, offset):
        if offset >= len(table) or b'\0' not in table[offset:]:
            raise ValueError('invalid ELF string')
        return table[offset:].split(b'\0', 1)[0].decode()
    section_names = [string(names, s[0]) for s in sections]
    symbols = {}
    for s in sections:
        if s[1] != 2:
            continue
        if s[9] != 24 or s[5] % 24 or s[6] >= len(sections):
            raise ValueError('invalid symtab')
        strings = sections[s[6]]
        strings = take(strings[4], strings[5])
        for p in range(s[4], s[4]+s[5], 24):
            n, info, other, sh, value, size = struct.unpack('<IBBHQQ', take(p, 24))
            name = string(strings, n)
            if name:
                symbols[name] = (value, size, sh, info)
    return b, h, sections, section_names, symbols, take

def build(recipe_path, compiler, output):
    output.mkdir(parents=True, exist_ok=True)
    # A failed rebuild must not leave a previously valid deployable package.
    for name in ('patch.json', 'build.json'):
        (output/name).unlink(missing_ok=True)
    recipe = json.loads(recipe_path.read_text(), object_pairs_hook=unique)
    if recipe['schema'] != 'shadps4.guest-functions.recipe.v1' or recipe['abi'] != 'x86_64-sysv':
        raise ValueError('unsupported recipe/ABI')
    identifier(recipe['id'])
    contract_tool = Path(__file__).with_name('recompile_contract.py')
    contract_spec = importlib.util.spec_from_file_location('recompile_contract', contract_tool)
    contract_module = importlib.util.module_from_spec(contract_spec)
    contract_spec.loader.exec_module(contract_module)
    recompile, reviewed_dependencies = contract_module.verify(recipe_path, recipe)
    if not 1 <= len(recipe['hooks']) <= 64:
        raise ValueError('expected 1..64 hooks')
    imports = list(SDK_IMPORTS)
    prototypes = ['#include "shad_guest.h"', '#include "shad_entry.h"']
    observers = []
    for header in recipe.get('headers', []):
        prototypes.append('#include ' + json.dumps(str((recipe_path.parent/header).resolve())))
    prototypes.append('#ifdef __cplusplus\nextern "C" {\n#endif')
    names = set()
    for hook in recipe['hooks']:
        for field in ('name', 'replacement', 'original'):
            identifier(hook[field])
        if hook['name'] in names or hook['original'] in imports:
            raise ValueError('duplicate hook/original')
        names.add(hook['name'])
        if hook['replacement'] == hook['original'] or hook['replacement'] in SDK_IMPORTS:
            raise ValueError('replacement/import alias')
        if not hook.get('evidence') or not 5 <= len(bytes.fromhex(hook['expected'])) <= 256:
            raise ValueError('hook needs ABI evidence and 5..256 expected bytes')
        mode = hook.get('mode', 'typed')
        if mode == 'entry-observer-x86_64-avx':
            callback = identifier(hook['observer'])
            if callback in (hook['replacement'], hook['original']) or callback in SDK_IMPORTS:
                raise ValueError('observer aliases entry/import')
            if hook['prototype'] != 'opaque-machine-entry':
                raise ValueError('entry observer must not invent an original C prototype')
            prototypes.append(f'void {callback}(const ShadGuestEntryContext*);')
            observers.append(hook)
        elif mode == 'typed':
            p = hook['prototype'].strip().rstrip(';')
            if not re.search(r'\b'+re.escape(hook['original'])+r'\s*\(', p) or len(p)>1024:
                raise ValueError('prototype must declare original import by name')
            prototypes += [p+';', re.sub(r'\b'+hook['original']+r'\b', hook['replacement'], p)+';']
        else:
            raise ValueError('unsupported hook mode')
        imports.append(hook['original'])
    bindings = recipe.get('bindings', [])
    if not isinstance(bindings, list) or len(bindings) > 128:
        raise ValueError('expected at most 128 guest bindings')
    data_bindings = set()
    assertions = []
    exports_requested = {h['replacement'] for h in recipe['hooks']} | set(recipe.get('exports', []))
    if exports_requested.intersection(imports):
        raise ValueError('export/import alias')
    for binding in bindings:
        name = identifier(binding['name'])
        if name in imports or name in exports_requested or name.startswith('shad_sdk_'):
            raise ValueError('guest binding aliases import/export')
        if not binding.get('evidence') or not isinstance(binding['offset'], int) or isinstance(binding['offset'], bool) or binding['offset'] < 0:
            raise ValueError('guest binding requires module offset and evidence')
        if binding['kind'] == 'function':
            p = binding['prototype'].strip().rstrip(';')
            if not re.search(r'\b'+re.escape(name)+r'\s*\(', p) or len(p) > 1024:
                raise ValueError('guest function requires typed prototype')
            if not 5 <= len(bytes.fromhex(binding['expected'])) <= 256:
                raise ValueError('guest function requires 5..256 expected bytes')
            prototypes.append(p+';')
        elif binding['kind'] == 'data':
            size, align = binding['size'], binding['alignment']
            if (type(size) is not int or type(align) is not int or not 0 < size <= 2**20 or
                    not 0 < align <= 4096 or align & (align-1) or binding['offset'] % align or
                    binding['access'] not in ('r', 'rw')):
                raise ValueError('invalid guest data geometry/access')
            t = binding['type']
            if not isinstance(t, str) or not t or len(t) > 512 or any(c in t for c in ';\n{}#='):
                raise ValueError('invalid guest data type')
            const = 'const ' if binding['access'] == 'r' else ''
            prototypes.append(f'extern {const}{t} *const {name} __attribute__((visibility("hidden")));')
            assertions += [f'SHAD_BIND_ASSERT(sizeof({t}) == {size}, "{name}: size mismatch");',
                           f'SHAD_BIND_ASSERT(SHAD_BIND_ALIGN({t}) <= {align}, "{name}: alignment mismatch");']
            data_bindings.add(name)
        else:
            raise ValueError('unknown guest binding kind')
        imports.append(name)
    prototypes.append('#ifdef __cplusplus\n}\n#define SHAD_BIND_ASSERT static_assert\n#define SHAD_BIND_ALIGN alignof\n#else\n#define SHAD_BIND_ASSERT _Static_assert\n#define SHAD_BIND_ALIGN _Alignof\n#endif')
    prototypes += assertions + ['#undef SHAD_BIND_ASSERT', '#undef SHAD_BIND_ALIGN']
    header = output/'shad_imports.h'
    header.write_text('\n'.join(prototypes)+'\n')
    asm = []
    for name in imports:
        if name not in data_bindings:
            asm += ['.text', '.p2align 4', f'.globl {name}', f'.type {name},@function',
                name+':', f'jmp *shad_import_slot_{name}(%rip)',
                f'.size {name}, .-{name}']
        asm += ['.section .shad_imports,"a",@progbits', '.p2align 3',
                f'.globl shad_import_slot_{name}', f'shad_import_slot_{name}:']
        if name in data_bindings:
            asm += [f'.globl {name}', f'.type {name},@object', f'.size {name},8', name+':']
        asm += ['.quad 0']
    asm += ['.section .note.GNU-stack,"",@progbits']
    (output/'imports.S').write_text('\n'.join(asm)+'\n')
    sources = [(recipe_path.parent/s).resolve() for s in recipe['sources']]
    sources += [SDK/'runtime.c', output/'imports.S']
    if observers:
        adapter = output/'entry_observers.S'
        adapter.write_text('#include ' + json.dumps(str(SDK/'entry_observer.S')) + '\n' +
            '\n'.join(f"SHAD_ENTRY_OBSERVER {h['replacement']}, {h['observer']}, {h['original']}" for h in observers) +
            '\n.section .note.GNU-stack,"",@progbits\n')
        sources.append(adapter)
    if len(sources)>66 or len(set(sources)) != len(sources):
        raise ValueError('duplicate/too many sources')
    commands = []
    dependencies = {recipe_path, Path(__file__).resolve(), SDK/'payload.ld', header}
    dependencies.update(reviewed_dependencies)
    dependencies.add(contract_tool)
    def run(args):
        args = list(map(str, args)); commands.append(args)
        return subprocess.check_output(args, stderr=subprocess.STDOUT, text=True)
    objects = []
    for i, source in enumerate(sources):
        if source.suffix not in ('.c', '.cpp', '.cc', '.S', '.s'):
            raise ValueError('only C/C++/assembly sources')
        obj = output/f'{i}.o'; dep = output/f'{i}.d'
        flags = ['--target=x86_64-none-elf', '-O2', '-g', '-ffreestanding', '-fPIE',
                 '-fno-builtin', '-fno-stack-protector', '-mno-red-zone', '-march=x86-64',
                 '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fvisibility=hidden',
                 '-nostdinc', '-Wall', '-Wextra', '-Werror', '-MMD', '-MF', dep, '-I', SDK/'include',
                 '-I', recipe_path.parent, '-I', output]
        if source.suffix in ('.cpp', '.cc'):
            flags += ['-std=c++20', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
                      '-include', header]
        elif source.suffix == '.c':
            flags += ['-std=c11']
            # SDK runtime has no title-specific ABI dependencies. A C++ title
            # header must not be force-included into this independent C TU.
            if source != SDK/'runtime.c':
                flags += ['-include', header]
        run([compiler, *flags, '-c', source, '-o', obj])
        dependencies.add(source)
        if dep.exists():
            # Clang make dependencies; this workspace also supports escaped spaces.
            import shlex
            dependencies.update(Path(p) for p in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
        objects.append(obj)
    linker = compiler.parent/'ld.lld'; elf = output/'patch.elf'
    run([linker, '-static', '--no-undefined', '--fatal-warnings', '--emit-relocs',
         '-e', recipe['hooks'][0]['replacement'], '-T', SDK/'payload.ld', '-o', elf, *objects])
    b, h, sections, section_names, symbols, take = read_elf(elf)
    segments = []
    for i in range(h[10]):
        if h[9] != 56: raise ValueError('invalid program headers')
        t, flags, offset, va, _, filesz, memsz, alignment = struct.unpack('<IIQQQQQQ', take(h[5]+i*56,56))
        if t in (2,3,7): raise ValueError('dynamic/TLS payload unsupported')
        if t != 1 or not memsz: continue
        if flags not in (5,6) or filesz > memsz or va % 4096 or va+memsz > 2**20:
            raise ValueError('invalid/oversized segment')
        blob = take(offset,filesz)
        segments.append({'offset':va,'size':(memsz+4095)&~4095,'executable':flags==5,
                         'hex':blob.hex(),'sha256':sha(blob)})
    if not segments or segments[0]['offset'] != 0 or not segments[0]['executable']:
        raise ValueError('missing initial code segment')
    rebase = []
    for s, name in zip(sections,section_names):
        if s[2]&2 and s[5]:
            if s[2]&0x400 or name.startswith(('.init','.fini','.ctors','.dtors','.eh_frame','.gcc_except')):
                raise ValueError('TLS/constructors/unwinding are unsupported: '+name)
        if s[1] not in (4,9) or not sections[s[7]][2]&2: continue
        if s[1]!=4 or s[9]!=24 or s[5]%24: raise ValueError('bad relocation table')
        for pos in range(s[4],s[4]+s[5],24):
            address, info, addend = struct.unpack('<QQq',take(pos,24)); kind = info&0xffffffff
            if kind == 1: rebase.append(address)
            elif kind not in (2,4): raise ValueError('unsupported relocation '+str(kind))
    exports = {}
    for name in {h['replacement'] for h in recipe['hooks']} | set(recipe.get('exports',[])):
        identifier(name)
        if name not in symbols or symbols[name][2] == 0 or symbols[name][3]&15 != 2:
            raise ValueError('missing function export: '+name)
        exports[name] = symbols[name][0]
    payload = {k:recipe[k] for k in ('id','title','module','module_sha256','hooks')}
    if 'executable_sha256' in recipe:
        payload['executable_sha256'] = recipe['executable_sha256']
    if bindings:
        payload['bindings'] = bindings
    payload.update(schema='shadps4.guest-functions.v1', abi='x86_64-sysv', sdk_version=1,
                   segments=segments, rebase64=rebase, exports=exports,
                   imports=[{'name':name,'slot':symbols['shad_import_slot_'+name][0]} for name in imports],
                   counters=recipe.get('counters',[]), logs=recipe.get('logs',[]), elf_sha256=sha(b))
    report = {'schema':'shadps4.guest-functions.build.v1','compiler':run([compiler,'--version']),
              'linker':run([linker,'--version']), 'commands':commands,
              'tool_sha256':{str(p):sha(p.read_bytes()) for p in (compiler,linker,compiler.parent/'llvm-objdump')},
              'sources':{str(p.resolve()):sha(p.read_bytes()) for p in sorted(dependencies)},
              'elf_sha256':sha(b), 'sdk_version':1}
    if recompile:
        report['recompile'] = recompile
    (output/'disassembly.txt').write_text(run([compiler.parent/'llvm-objdump','-d','-r',elf]))
    encoded = (json.dumps(payload,indent=2,sort_keys=True)+'\n').encode()
    report['package_sha256'] = sha(encoded)
    (output/'patch.json').write_bytes(encoded)
    (output/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    print('GUEST_FUNCTIONS_BUILD_PASS '+sha(encoded))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--recipe',type=Path,required=True)
    p.add_argument('--clang',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    try: build(a.recipe.resolve(), a.clang.resolve(), a.output.resolve())
    except (OSError,ValueError,KeyError,IndexError,struct.error,subprocess.CalledProcessError) as e:
        for n in ('patch.json','build.json'): (a.output/n).unlink(missing_ok=True)
        print(getattr(e,'output','') or str(e),file=sys.stderr); return 1
    return 0
if __name__=='__main__': sys.exit(main())
