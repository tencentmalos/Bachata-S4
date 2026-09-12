#!/usr/bin/env python3
"""NDK source census. Does not link the host or execute it on a device.
Reads root CMake's VIDEO_CORE/SHADER_RECOMPILER lists; generates real host-shader
string headers using the existing generator. Every command/exit/log is retained.
"""
import argparse, concurrent.futures, hashlib, json, pathlib, re, shlex, subprocess, sys, time
p = argparse.ArgumentParser()
p.add_argument('--ndk', type=pathlib.Path, required=True)
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--jobs', type=int, default=4)
p.add_argument('--emit-objects', action='store_true')
p.add_argument('--set', choices=['graphics','spine'], default='graphics')
p.add_argument('sources', nargs='*')
a = p.parse_args()
if a.jobs < 1: p.error('--jobs must be positive')
root = pathlib.Path(__file__).resolve().parents[2]
out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
compiler = next(iter(sorted(a.ndk.glob('toolchains/llvm/prebuilt/*/bin/clang++'))), None)
if compiler is None: raise SystemExit('NDK compiler missing')
compiler = compiler.resolve()
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def block(text, name):
    m = re.search(r'set\('+re.escape(name)+r'\s+(.*?)\)', text, re.S)
    if not m: raise ValueError('Missing CMake list '+name)
    return m[1]
source_diff = subprocess.check_output(['git','diff','--','src'],cwd=root,text=True)
source_files = sorted(p for p in (root/'src').rglob('*') if p.is_file())
source_manifest = {str(p.relative_to(root)):sha(p) for p in source_files}
cmake = (root/'CMakeLists.txt').read_text()
groups = {name: re.findall(r'src/[^\s()]+\.cpp', block(cmake, name)) for name in ('VIDEO_CORE','SHADER_RECOMPILER')}
spine = ['src/core/'+s+'.cpp' for s in ['linker','module','tls','memory','address_space','aerolib/aerolib','aerolib/stubs','loader/elf','loader/symbols_resolver','loader/dwarf']]
sources = a.sources or (spine if a.set == 'spine' else [s for files in groups.values() for s in files])
if not sources or len(sources) != len(set(sources)): raise SystemExit('empty or duplicate source list')
shader_dir = root/'src/video_core/host_shaders'
headers = out/'generated/include'
for src in block((shader_dir/'CMakeLists.txt').read_text(), 'SHADER_FILES').split():
    dest = headers/'video_core/host_shaders'/(src.replace('.', '_')+'.h')
    subprocess.run(['cmake','-P',str(shader_dir/'StringShaderHeader.cmake'),str(shader_dir/src),str(dest),str(shader_dir/'source_shader.h.in')], cwd=root, check=True, capture_output=True)
inc_path = root/'scripts/android/host-ndk-include-set.txt'
flags = shlex.split(inc_path.read_text()) + ['-I','externals/fmt/include','-I',str(headers)]
# Explicit NDK profile, not desktop ABI flags. Tracy OFF means macro undefined.
flags += ['-DUSE_OS_TZDB=1','-DARCH_ARM64=1','-DAL_LIBTYPE_STATIC','-DBOOST_ASIO_STANDALONE','-DHAS_STRING_VIEW=1','-DNOMINMAX','-DONLY_C_LOCALE=0','-DPUGIXML_NO_EXCEPTIONS','-DSPDLOG_FUNCTION=__func__','-DZYCORE_STATIC_BUILD','-DZYDIS_STATIC_BUILD','-DNDEBUG','-DVK_USE_PLATFORM_ANDROID_KHR','-DIMGUI_USER_CONFIG="imgui/imgui_config.h"']
# Generate miniz's actual export header with the NDK toolchain, not a copied
# desktop header or a hand-written substitute. No external sources are changed.
if 'src/video_core/cache_storage.cpp' in sources:
    miniz_out = out/'miniz-config'
    command = ['cmake','-S',str(root/'externals/miniz'),'-B',str(miniz_out),'-G','Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(a.ndk.resolve()/'build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-33',
        '-DBUILD_TESTS=OFF','-DBUILD_EXAMPLES=OFF','-DINSTALL_PROJECT=OFF']
    config = subprocess.run(command,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/'miniz-config.log').write_bytes(config.stdout)
    if config.returncode: raise SystemExit('miniz configure failed: '+str(out/'miniz-config.log'))
    flags += ['-I',str(miniz_out),'-DMINIZ_STATIC_DEFINE']
base = [str(compiler),'--target=aarch64-linux-android33','-std=gnu++2b','-fPIC',*flags]
def run(src):
    command = [*base, '-c', src, '-o', str(out/(src.replace('/','_')+'.o'))] if a.emit_objects else [*base,'-fsyntax-only',src]
    start = time.monotonic()
    try:
        r = subprocess.run(command,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
        code, log = r.returncode, r.stdout
    except subprocess.TimeoutExpired as e:
        code, log = 124, (e.stdout or b'')+b'\nREVIEW TIMEOUT 120s\n'
    log_name = src.replace('/','_')+'.log'; (out/log_name).write_bytes(log)
    return dict(source=src,source_sha256=sha(root/src) if (root/src).is_file() else None,command=command,returncode=code,seconds=round(time.monotonic()-start,3),log=log_name)
with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool: results = list(pool.map(run,sources))
final_diff = subprocess.check_output(['git','diff','--','src'],cwd=root,text=True)
if final_diff != source_diff or any(sha(root/p) != h for p,h in source_manifest.items()): raise SystemExit('Sources changed during the census; rerun against stable source')
summary = dict(source_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),compiler=str(compiler),compiler_version=subprocess.check_output([str(compiler),'--version'],text=True),include_sha256=sha(inc_path),cmake_sha256=sha(root/'CMakeLists.txt'),source_manifest=source_manifest, mode='object' if a.emit_objects else 'syntax_only', source_diff=source_diff,groups={g:len(v) for g,v in groups.items()},pass_count=sum(r['returncode']==0 for r in results),fail_count=sum(r['returncode']!=0 for r in results),results=results)
(out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
print(f"pass={summary['pass_count']} fail={summary['fail_count']}",flush=True)
for r in results:
    if r['returncode']:
        print(r['source'], 'exit',r['returncode'])
        print('\n'.join(line for line in (out/r['log']).read_text(errors='replace').splitlines() if 'error:' in line)[:1600])
sys.exit(1 if summary['fail_count'] else 0)
