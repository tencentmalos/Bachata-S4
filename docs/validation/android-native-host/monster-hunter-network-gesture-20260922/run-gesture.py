from pathlib import Path
import subprocess,json,hashlib,shlex,time
r=Path(__file__).resolve().parent;repo=Path.cwd();remote='/data/local/tmp/mh-gesture-'+str(time.time_ns());a=['adb','-s','9c2841a4']
def run(args):return subprocess.run(list(map(str,args)),capture_output=True,text=True,check=True)
def shell(s):return run(a+['shell',s]).stdout
files=[repo/'build/validation/monster-hunter-20260921/runtime-tests/out/runtime/production_runtime_runner',repo/'build/android-host-api33/native/libshadps4_host.so',Path('/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'),r/'gesture-main.elf',r/'gesture-game-provider.elf',r/'gesture-libc-main.elf',repo/'build/validation/monster-hunter-20260921/modules/CUSA34119/sce_module/libc.prx',Path('/Users/bytedance/game/ps4/firmware/11.00_sys_modules/libSceLibcInternal.sprx'),Path('/Users/bytedance/game/ps4/firmware/11.00_sys_modules/libSceSystemGesture.sprx'),repo/'build/validation/monster-hunter-20260921/runtime-tests/fixtures/system.sprx']
m={'remote':remote,'artifacts':[],'results':[]}
assert shell(f'test ! -e {remote} && mkdir -p {remote}/user/sys_modules').strip()==''
try:
 for p in files:
  run(a+['push',p,remote+'/'+p.name]);sha=hashlib.sha256(p.read_bytes()).hexdigest();assert shell(f'sha256sum {remote}/{p.name}').split()[0]==sha;m['artifacts'].append({'path':str(p),'sha256':sha})
 cases=[('missing','gesture-main.elf',None,None,'fault','operation='),('wrong-identity','gesture-main.elf','system.sprx',None,'reject',''),('firmware-game-libc','gesture-libc-main.elf','libSceSystemGesture.sprx',None,'return','51966'),('game-precedence','gesture-game-provider.elf','system.sprx','libSceSystemGesture.sprx','return','51966')]
 for label,main,system,game,mode,expected in cases:
  shell(f'rm -f {remote}/user/sys_modules/libSceSystemGesture.sprx {remote}/fixture_dependency.sprx')
  shell(f'rm -f {remote}/user/sys_modules/libSceLibcInternal.sprx')
  if system:shell(f'cp {remote}/{system} {remote}/user/sys_modules/libSceSystemGesture.sprx')
  if game:shell(f'cp {remote}/{game} {remote}/fixture_dependency.sprx')
  p=subprocess.run(a+['shell',f'cd {remote} && LD_LIBRARY_PATH={remote} timeout -s KILL 40 ./production_runtime_runner {remote}/user {remote}/{main} {mode} {shlex.quote(expected)}'],capture_output=True,text=True,timeout=50)
  (r/('gesture-v4-'+label+'.log')).write_text(p.stdout+p.stderr)
  passed=p.returncode==0 and 'rounds=3' in p.stdout
  if label=='wrong-identity':passed=passed and 'system guest provider identity mismatch' in p.stdout
  m['results'].append({'case':label,'exit':p.returncode,'passed':passed});(r/'gesture-tests-v4.json').write_text(json.dumps(m,indent=2)+'\n');print(label,passed,p.stdout[-1200:],flush=True)
  if not passed:raise RuntimeError(label+' failed')
finally:
 m['cleanup']=shell(f'rm -rf {remote}');(r/'gesture-tests-v4.json').write_text(json.dumps(m,indent=2)+'\n')
