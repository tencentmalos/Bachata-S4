from pathlib import Path
import subprocess, concurrent.futures, json
root=Path.cwd(); work=Path('/tmp/shadps4-g2-exit-repair-20260909'); work.mkdir(exist_ok=True)
ndk='/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865'
def build(name, opts):
    out=work/name
    commands=[['cmake','-S',str(root/'cmake/fex'),'-B',str(out),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',*opts],['cmake','--build',str(out),'-j','8']]
    result=[]
    with (work/(name+'.txt')).open('w') as log:
        for cmd in commands:
            log.write(json.dumps(cmd)+'\n');log.flush()
            r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT);result.append(r.returncode)
            if r.returncode: break
    return name,result
jobs=[('build',[f'-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake','-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-35','-DV0_ENABLE_FEX=ON',f'-DFEX_BUILD_DIR={root}/build/fexcore-android',f'-DV0_FIXTURE_DIR={work}/fixtures']),('host-build',[]),('production',['-DV0_BUILD_TESTS=OFF'])]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    for name,result in pool.map(lambda job:build(*job),jobs): print(name,result,flush=True)
