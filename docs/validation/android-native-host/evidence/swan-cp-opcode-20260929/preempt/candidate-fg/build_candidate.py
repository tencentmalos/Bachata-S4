from pathlib import Path
import subprocess,hashlib,json,shutil,os,zipfile
root=Path.cwd();out=root/'build/validation/swan-cp-preempt-20260929/candidate-fg';lib=root/'build/turnip-api33/native/src/freedreno/vulkan/libvulkan_freedreno.so';sha=hashlib.sha256(lib.read_bytes()).hexdigest();shutil.copy2(lib,out/'vulkan.ad07xx.so')
paths=[root/'src/video_core/renderer_vulkan/vk_driver_android.cpp',root/'android/shadps4-app/core/runtime/src/main/kotlin/com/shadps4/android/runtime/session/AndroidTurnip.kt'];originals={p:p.read_bytes() for p in paths}
asset=root/'android/shadps4-app/app/build/generated/nativeTurnipAssets/native-turnip-mainline';stable='ea4853bf58cdee3d49369706249090899cb4ebb17b8f0ca23685918912f0b6a1';patch=subprocess.check_output(['git','-C','references/mesa-turnip','diff']);(out/'turnip-combined.patch').write_bytes(patch)
try:
 for p,data in originals.items():
  s=data.decode();assert stable in s;p.write_text(s.replace(stable,sha))
 shutil.copy2(lib,asset/'vulkan.ad07xx.so');p=asset/'identity.json';m=json.loads(p.read_text());m.update(library_sha256=sha,release='local-86ca-mapper5-kgsl-preempt-styles',source_dirty=True,patch_sha256=hashlib.sha256(patch).hexdigest())
 for k in list(m):
  if 'url' in k or 'archive' in k:m.pop(k)
 p.write_text(json.dumps(m,indent=2)+'\n');(out/'identity.json').write_bytes(p.read_bytes())
 with (out/'build-host-v2.log').open('w') as f:subprocess.run(['cmake','--build','build/android-host-api33/native','--target','shadps4_host','-j6'],check=True,stdout=f,stderr=subprocess.STDOUT)
 env=dict(os.environ,JAVA_HOME='/Library/Java/JavaVirtualMachines/jdk-17.jdk/Contents/Home')
 with (out/'build-apk-v2.log').open('w') as f:subprocess.run(['./gradlew',':app:assemblePlaystoreDebug','-x',':app:prepareMainlineTurnip','--console=plain'],cwd=root/'android/shadps4-app',env=env,check=True,stdout=f,stderr=subprocess.STDOUT)
 apk=out/'shadps4-b0778a7f-kgsl-preempt-styles.apk';shutil.copy2(root/'android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk',apk)
 with zipfile.ZipFile(apk) as z: hashes={n:hashlib.sha256(z.read(n)).hexdigest() for n in z.namelist() if n.endswith(('vulkan.ad07xx.so','libshadps4_host.so'))}
 assert hashes['assets/native-turnip-mainline/vulkan.ad07xx.so']==sha
 ident={'apk_sha256':hashlib.sha256(apk.read_bytes()).hexdigest(),'entries':hashes};(out/'apk-identity.json').write_text(json.dumps(ident,indent=2));(out/'driver-sha256.txt').write_text(sha+'\n');print(json.dumps(ident))
finally:
 for p,data in originals.items():p.write_bytes(data)
 subprocess.run(['python3','scripts/android/prepare-bionic-turnip','--lock','runtime/locks/turnip-bionic-mainline.json','--out',str(asset)],check=True)
