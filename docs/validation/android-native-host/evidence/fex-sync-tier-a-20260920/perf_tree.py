import re,sys,os
from pathlib import Path
PCT=re.compile(r'--([\d.]+)%-- ')
def show(path, minpct=1.5):
    t=Path(path).read_text(encoding="utf-8",errors="replace")
    print("#####",Path(path).name, re.search(r"Samples: (\d+)",t).group(1),"samples")
    stack=[]
    for line in t.splitlines():
        m=PCT.search(line)
        if not m or line.lstrip().startswith(('Children','Cmdline')): continue
        prefix=line[:m.start()]
        if prefix.strip(' |')!='': continue
        level=len(prefix)//5; rel=float(m.group(1))/100; sym=line[m.end():]
        sym=sym.replace('(anonymous namespace)::','').replace('std::__ndk1::','')
        sym=re.sub(r'\(.*','',sym).replace('Core::HostRuntime::','HR::').replace('Core::GuestCpu::','GC::').replace('Fex::FexCpuContext::','FEX::')
        sym=re.sub(r'\.llvm\.\d+','',sym); sym=re.sub(r'^_ZNSt.*thread_proxy.*','<thread_proxy>',sym)
        stack=stack[:level]
        parent=stack[-1][0] if stack else 1.0
        while len(stack)<level: stack.append((parent,'?'))
        absp=parent*rel
        stack.append((absp,sym))
        if absp*100>=minpct and not re.search(r'el0|svc|__pthread_start|__start_thread|thread_proxy|\[hit in',sym):
            print(f"{'  '*max(0,level-2)}{absp*100:6.2f}% {sym[:105]}")
for p in sys.argv[1:]:
    show(p, minpct=float(os.environ.get('MINPCT','1.5')))
