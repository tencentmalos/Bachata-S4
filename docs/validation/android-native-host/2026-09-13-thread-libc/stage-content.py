import subprocess,tarfile,tempfile,pathlib,time,json,hashlib
root=pathlib.Path.cwd();source=root/'build/wp1-review/tmnt-content';relative='validation/tmnt-wp2-'+str(time.time_ns());adb=['/Users/bytedance/Library/Android/sdk/platform-tools/adb','-s','9c2841a4']
subprocess.run(adb+['shell','run-as','com.shadps4.android','mkdir','-p','files/'+relative],check=True)
with tempfile.TemporaryFile() as f:
 with tarfile.open(fileobj=f,mode='w') as tar:
  for path in source.rglob('*'):
   if path.is_file():tar.add(path,arcname=str(path.relative_to(source)),recursive=False)
 f.seek(0)
 subprocess.run(adb+['shell','-T','run-as','com.shadps4.android','tar','-xf','-','-C','files/'+relative],stdin=f,check=True)
files=[]
for path in source.rglob('*'):
 if not path.is_file():continue
 name=str(path.relative_to(source));sha=hashlib.sha256(path.read_bytes()).hexdigest()
 remote=subprocess.check_output(adb+['shell','run-as','com.shadps4.android','sha256sum','files/'+relative+'/'+name],text=True).split()[0]
 assert sha==remote
 files.append({'path':name,'sha256':sha,'size':path.stat().st_size})
(root/'build/thread-attrs-review/selected-content.json').write_text(json.dumps({'relative':relative,'files':files,'scope':'selected base+update executable/modules/param.sfo only; not full assets'},indent=2))
print(relative)
