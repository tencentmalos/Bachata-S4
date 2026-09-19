#!/usr/bin/env python3
"""Build the authored production-loader/APK fixture and its exact patch package."""
import argparse, importlib.util, json, shutil, struct, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--ndk',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[3];src=Path(__file__).resolve().parent
tool=next((a.ndk/'toolchains/llvm/prebuilt').glob('*'))/'bin';out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('builder',root/'tools/guest-functions/build.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
subprocess.run(['python3',str(src/'make_fixture.py'),'--clang',str(tool/'clang'),'--out',str(out/'mechanism')],check=True)
subprocess.run([str(tool/'clang'),'--target=x86_64-none-elf','-c',str(src/'app_entry.S'),'-o',str(out/'app_entry.o')],check=True)
subprocess.run([str(tool/'ld.lld'),'-static','-e','app_entry','-T',str(root/'guest/custom/v1/payload.ld'),'-o',str(out/'app.elf'),str(out/'mechanism/target.o'),str(out/'app_entry.o')],check=True)
data,h,sections,names,symbols,take=b.read_elf(out/'app.elf')
ph=struct.unpack_from('<IIQQQQQQ',data,h[5]);blob=take(ph[2],ph[5]);assert len(blob)<0x4000
subprocess.run(['python3',str(root/'scripts/android/generate-production-runtime-fixture'),'--ndk',str(a.ndk),'--out',str(out/'eboot.bin')],check=True)
image=bytearray((out/'eboot.bin').read_bytes());image[0x4000:0x8000]=blob.ljust(0x4000,b'\x90');struct.pack_into('<Q',image,24,symbols['app_entry'][0]);(out/'eboot.bin').write_bytes(image)
recipe=json.loads((out/'mechanism/recipe.json').read_text());recipe.update(module='eboot.bin',module_sha256=b.sha(image),title='CUSA99992')
for hook in recipe['hooks']:
    address,size,_,_=symbols['target_'+hook['name']];hook.update(offset=address,expected=blob[address:address+min(size,32)].hex())
(out/'recipe.json').write_text(json.dumps(recipe,indent=2)+'\n');b.build(out/'recipe.json',tool/'clang',out/'package')
keys=b'TITLE_ID\0APP_VER\0TITLE\0';table=bytearray();values=bytearray()
for key,value in zip((b'TITLE_ID',b'APP_VER',b'TITLE'),(b'CUSA99992\0',b'01.00\0',b'Guest patch contract\0')):
    table+=struct.pack('<HHIII',keys.index(key+b'\0'),0x204,len(value),len(value),len(values));values+=value
sfo=b'\x00PSF'+struct.pack('<IIII',0x101,20+len(table),20+len(table)+len(keys),3)+table+keys+values
(out/'param.sfo').write_bytes(sfo)
print('APK_PATCH_FIXTURE_PASS',b.sha(image))
