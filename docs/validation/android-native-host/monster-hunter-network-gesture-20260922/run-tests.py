from pathlib import Path
import subprocess,json,hashlib,time,sys
root=Path(__file__).resolve().parent
label=sys.argv[1]
name=sys.argv[2] if len(sys.argv)>2 else 'guest_file_io_tests'
remote='/data/local/tmp/mh-temp-'+str(time.time_ns())
adb=['adb','-s','9c2841a4']
files=[Path('build/android-host-api33/native')/name,Path('build/android-host-api33/native/libshadps4_host.so'),Path('/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so')]
def run(a):return subprocess.run(a,capture_output=True,text=True,check=True)
m={'remote':remote,'artifacts':[]}
run(adb+['shell',f'test ! -e {remote} && mkdir {remote}'])
try:
 for p in files:
  run(adb+['push',str(p),remote+'/'+p.name]);sha=hashlib.sha256(p.read_bytes()).hexdigest()
  assert run(adb+['shell','sha256sum',remote+'/'+p.name]).stdout.split()[0]==sha
  m['artifacts'].append({'path':str(p),'sha256':sha})
 argument=f'{remote}/data' if name=='guest_file_io_tests' else ''
 p=subprocess.run(adb+['shell',f'cd {remote} && LD_LIBRARY_PATH={remote} timeout -s KILL 45 {remote}/{name} {argument}'],capture_output=True,text=True,timeout=55)
 (root/(label+'.log')).write_text(p.stdout+p.stderr);m['exit']=p.returncode
 print(p.stdout[-2200:]+p.stderr[-500:],flush=True)
finally:
 m['cleanup']=run(adb+['shell',f'rm -rf {remote}']).stdout
 (root/(label+'.json')).write_text(json.dumps(m,indent=2)+'\n')
sys.exit(m.get('exit',1))
