from pathlib import Path
import subprocess,shlex
root=Path.cwd(); out=root/'build/native-locks-20260917'; build=root/'build/graphics-toolkit-review/clock-native'
inc=out/'baseline-include/core/host_runtime';inc.mkdir(parents=True,exist_ok=True)
(inc/'guest_mutex.h').write_bytes((out/'guest_mutex.before.h').read_bytes())
cmds=subprocess.check_output(['ninja','-t','commands','guest_native_mutex_tests'],cwd=build,text=True).splitlines()
compile=next(shlex.split(c) for c in cmds if ' -c ' in c and 'guest_native_mutex_tests.cpp' in c)
obj=compile[compile.index('-o')+1];baseline_obj=str(out/'baseline-native.o');compile[compile.index('-o')+1]=baseline_obj
compile[1:1]=['-I'+str(out/'baseline-include')]
if '-MF' in compile:compile[compile.index('-MF')+1]=str(out/'baseline-native.d')
subprocess.run(compile,cwd=build,check=True)
link=next(shlex.split(c.split(' && ')[1]) for c in cmds if ' -o guest_native_mutex_tests ' in c)
link=[baseline_obj if x==obj else x for x in link];link[link.index('-o')+1]=str(out/'baseline-native-tests')
link=[x.replace('CMakeFiles/guest_native_mutex_tests.dir/link.d',str(out/'baseline-link.d')) for x in link]
subprocess.run(link,cwd=build,check=True)
print('baseline built from preserved old header; test source identical')
