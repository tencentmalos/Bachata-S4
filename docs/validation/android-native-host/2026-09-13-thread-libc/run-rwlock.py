import pathlib,subprocess,json,hashlib,time,sys
root=pathlib.Path.cwd();out=root/'build/thread-attrs-review'/sys.argv[1];out.mkdir(exist_ok=False)
adb=['/Users/bytedance/Library/Android/sdk/platform-tools/adb','-s','9c2841a4'];remote='/data/local/tmp/shadps4-drain-'+str(time.time_ns())
def run(a):return subprocess.check_output(a,text=True,stderr=subprocess.STDOUT)
files=[root/'build/wp1-review/device/guest_rwlock_tests',pathlib.Path('/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so')]
m={'scope':'only rwlock/libc-policy domain; no full regression','source':run(['git','rev-parse','HEAD']).strip(),'sources':{},'remote':remote,'files':{}}
for p in ['src/core/host_runtime/guest_mutex.h','src/core/host_runtime/guest_rwlock.h','src/core/host_runtime/guest_libc_policy.h','tests/host_runtime/guest_rwlock_tests.cpp']:m['sources'][p]=hashlib.sha256((root/p).read_bytes()).hexdigest()
try:
 run(adb+['shell','mkdir',remote])
 for p in files:
  sha=hashlib.sha256(p.read_bytes()).hexdigest();m['files'][p.name]=sha;run(adb+['push',str(p),remote+'/'+p.name]);assert run(adb+['shell','sha256sum',remote+'/'+p.name]).split()[0]==sha
 run(adb+['shell','chmod','700',remote+'/guest_rwlock_tests'])
 result=subprocess.run(adb+['shell',f'LD_LIBRARY_PATH={remote} timeout -s KILL 30 {remote}/guest_rwlock_tests'],capture_output=True,text=True,timeout=35)
 (out/'result.log').write_text(result.stdout+result.stderr);m['exit']=result.returncode;print(result.stdout+result.stderr);print('EXIT',result.returncode)
finally:
 m['cleanup_exit']=subprocess.run(adb+['shell','rm','-rf',remote],capture_output=True).returncode;(out/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
