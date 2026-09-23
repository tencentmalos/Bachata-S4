from pathlib import Path
import subprocess,json,hashlib,shlex,time
repo=Path.cwd();out=repo/'build/validation/monster-hunter-20260921/runtime-tests';ndk=Path('/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865');remote='/data/local/tmp/mh-libc-bootstrap-20260921';adb=['adb','-s','9c2841a4']
def run(args):return subprocess.run(list(map(str,args)),capture_output=True,text=True,check=True)
def shell(s):return run(adb+['shell',s]).stdout
fixtures=out/'fixtures';fixtures.mkdir(exist_ok=True)
variants=[('policy-main.elf',['--libc-policy']),('policy-system-main.elf',['--libc-policy','--system-libc']),('libc.prx',['--libc-policy','--module']),('system.sprx',['--libc-policy','--module','--system-libc']),('bad-system.sprx',['--libc-policy','--module','--system-libc','--bad-pointer']),('bad-game.prx',['--libc-policy','--module','--bad-pointer']),('bootstrap.elf',['--with-dependency','--libc']),('bootstrap.prx',['--module','--libc']),('bootstrap-wait.prx',['--module','--libc','--wait'])]
for name,args in variants:run(['python3',repo/'scripts/android/generate-production-runtime-fixture','--ndk',ndk,'--out',fixtures/name,*args])
# The policy main intentionally returns the provider's distinct 260/516 tag.
files=[out/'out/runtime/production_runtime_runner',repo/'build/android-host-api33/native/libshadps4_host.so',ndk/'toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so',*fixtures.glob('*')]
manifest={'remote':remote,'serial':'9c2841a4','artifacts':[],'results':[]};(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
assert shell(f'test ! -e {remote} && mkdir -p {remote}/user/sys_modules').strip()==''
try:
 for p in files:
  if not p.is_file():continue
  run(adb+['push',p,remote+'/'+p.name]);sha=hashlib.sha256(p.read_bytes()).hexdigest();assert shell(f'sha256sum {remote}/{shlex.quote(p.name)}').split()[0]==sha
  manifest['artifacts'].append({'path':str(p),'sha256':sha})
 shell(f'chmod 700 {remote}/production_runtime_runner')
 cases=[('fallback','libc.prx',None,'policy-main.elf','return','260'),('system-priority','libc.prx','system.sprx','policy-main.elf','return','516'),('system-fail-closed','libc.prx','bad-system.sprx','policy-main.elf','fault','guest _malloc_init failed'),('fallback-restored','libc.prx',None,'policy-main.elf','return','260'),('game-constructor-fail','bad-game.prx',None,'policy-main.elf','fault','module initialization failed'),('bootstrap-tls','bootstrap.prx',None,'bootstrap.elf','return','51966'),('bootstrap-cancel','bootstrap-wait.prx',None,'bootstrap.elf','cancel','51966')]
 for name,game,system,main,mode,expect in cases:
  if game!='libc.prx':shell(f'cp {remote}/{game} {remote}/libc.prx')
  else:run(adb+['push',fixtures/'libc.prx',remote+'/libc.prx'])
  shell(f'rm -f {remote}/user/sys_modules/libSceLibcInternal.sprx')
  if system:shell(f'cp {remote}/{system} {remote}/user/sys_modules/libSceLibcInternal.sprx')
  p=subprocess.run(adb+['shell',f'cd {remote} && LD_LIBRARY_PATH={remote} timeout -s KILL 30 ./production_runtime_runner {remote}/user {remote}/{main} {mode} {shlex.quote(expect)}'],capture_output=True,text=True,timeout=40)
  (out/(name+'.log')).write_text(p.stdout+p.stderr)
  passed=p.returncode==0 and f'PRODUCTION_RUNTIME_PASS mode={mode} rounds=3' in p.stdout
  manifest['results'].append({'case':name,'exit':p.returncode,'passed':passed,'rounds':p.stdout.count(' OUTCOME ')})
  (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(name,passed,flush=True)
  if not passed:raise RuntimeError(name+' failed')
finally:
 manifest['cleanup']=shell(f'rm -rf {remote}');(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
