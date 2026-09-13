import subprocess,pathlib,json,hashlib,re,tempfile,zipfile
r=pathlib.Path.cwd();o=r/'build/thread-attrs-review/apk-twelfth';o.mkdir(exist_ok=False);adb=['/Users/bytedance/Library/Android/sdk/platform-tools/adb','-s','9c2841a4']
def run(args,timeout=150):
 p=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
 if p.returncode:raise RuntimeError(p.stdout+p.stderr)
 return p.stdout
sha=lambda p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
manifest={'source':run(['git','rev-parse','HEAD']).strip(),'scope':'focused thread attributes, libc provider policy and real TMNT boundary; no full regression','runs':[]}
try:
 apk=r/'android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk';test=r/'android/shadps4-app/app/build/outputs/apk/androidTest/playstore/debug/app-playstore-debug-androidTest.apk'
 manifest['artifacts']={str(p.relative_to(r)):sha(p) for p in [apk,test,r/'build/android-host-api33/native/libshadps4_host.so']}
 manifest['sources']={p:sha(r/p) for p in run(['git','ls-files','--modified','--others','--exclude-standard','src','tests','android','scripts','cmake']).splitlines() if (r/p).is_file()}
 manifest['identity']=run(adb+['shell','getprop','ro.product.model']).strip()
 with zipfile.ZipFile(apk) as z,tempfile.TemporaryDirectory() as td:
  for name in ['libshadps4_host.so','libshadps4_fex_session.so']:
   p=pathlib.Path(td)/name;p.write_bytes(z.read('lib/arm64-v8a/'+name));txt=run(['/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf','-n',str(p)]);manifest[name]={'sha256':sha(p),'build_id':re.search(r'Build ID: (\w+)',txt).group(1)}
 run(adb+['install','-r',str(apk)]);run(adb+['install','-r',str(test)])
 rel=json.loads((r/'build/thread-attrs-review/selected-content.json').read_text())['relative']
 for name,selector,args in [('attrs','ThreadAttributeRuntimeInstrumentedTest',[]),('libc','LibcPolicyRuntimeInstrumentedTest',[]),('tmnt','RenderedRuntimeInstrumentedTest#realContentUsesSessionRendererAcrossThreeRestarts',['-e','contentRelativePath',rel])]:
  run(adb+['shell','am','force-stop','com.shadps4.android'])
  start=run(adb+['shell',"date '+%m-%d %H:%M:%S.000'"]).strip()
  output=run(adb+['shell','am','instrument','-w','-r','-e','class','com.shadps4.android.'+selector]+args+['com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner'])
  (o/(name+'-junit.log')).write_text(output)
  logs=run(adb+['logcat','-d','-v','threadtime','-T',start,'-s','ThreadAttributeAcceptance:I','LibcPolicyAcceptance:I','RenderedRuntimeAcceptance:I','ProductionRuntime:I','AndroidRuntime:E','DEBUG:I','*:S']);(o/(name+'-logcat.txt')).write_text(logs)
  ok='OK (1 test)' in output and 'FAILURES!!!' not in output
  manifest['runs'].append({'name':name,'junit_pass':ok,'scope':'synthetic' if name in ('attrs','libc') else 'boundary observation only'})
  print(name,ok,output[-1300:],flush=True);print('\n'.join(l for l in logs.splitlines() if 'round=' in l or 'ThreadAttributeAcceptance:' in l or 'LibcPolicyAcceptance:' in l),flush=True)
  assert ok
 manifest['status']='FOCUSED_PASS_BOUNDARY_OBSERVED'
except Exception as e:manifest['status']='FAIL';manifest['error']=str(e);raise
finally:(o/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
