import sys,json,time,os,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent));from mcp_stdio import Client
mode=sys.argv[1]; root=Path(os.environ.get('FEX_EVIDENCE_DIR') or tempfile.mkdtemp(prefix='fex-mixed-evidence-'));root.mkdir(parents=True,exist_ok=True)
c=Client('/Users/bytedance/workspace/doubao_wrapper/native-debugger');sid=None;events=[]
try:
 names=[x['name'] for x in c.init()['tools']];assert 'guest_debug.read_stack' in names
 def call(name,**args):
  result=c.call('guest_debug.'+name,args);events.append({'tool':name,'args':args,'result':result})
  print(name,result.get('succeeded'),result.get('errorCode',''),flush=True);return result
 def ok(name,**args):
  r=call(name,**args);assert r['succeeded'],r;return r
 start=ok('start_session',emulator='shadps4-fex',serial='9c2841a4',port=24681,remotePort=24680,expectedArchitecture='i386:x86-64',byteOrder='little',stopOnAttach=True,dryRun=False,stopLeaseMs=30000)
 sid=start['session']['sessionId'];paused=ok('control',sessionId=sid,action='pause',waitForStopMs=1000)
 epoch=paused['session']['stopEpoch']
 initial=ok('read_stack',sessionId=sid,stopEpoch=epoch,threadId='1');assert initial['mixedStack']['frames'][0]['kind']=='guest'
 if mode=='watch':
  for access,expected in [('write','write'),('read','read')]:
   added=ok('manage_breakpoints',sessionId=sid,operation='add',stopEpoch=epoch,address='0xff8030000',type=access,length=8)
   bp=added['session']['breakpoints'][-1]['breakpointId']
   hit=ok('control',sessionId=sid,action='continue',stopEpoch=epoch,waitForStopMs=1500)
   epoch=hit['session']['stopEpoch'];w=hit['session']['watchHit']
   assert w['access']==expected and w['accessAddress']=='0xff8030000' and w['accessBytes']==8,w
   regs=ok('read_registers',sessionId=sid,stopEpoch=epoch,registerNames=['rip','rax','rcx'])
   assert int(regs['registers'][0]['numericHex'],16)>int(w['instructionPc'],16)
   ok('manage_breakpoints',sessionId=sid,operation='remove',stopEpoch=epoch,breakpointId=bp)
  added=ok('manage_breakpoints',sessionId=sid,operation='add',stopEpoch=epoch,address='0xff8030000',type='read',length=8)
  refused=ok('control',sessionId=sid,action='continue',stopEpoch=epoch,waitForStopMs=1500)
  epoch=refused['session']['stopEpoch']
  assert refused['session']['state']=='stopped' and refused['session']['stopReason']=='unsupported',refused
  ok('read_stack',sessionId=sid,stopEpoch=epoch,threadId='1')
  ok('manage_breakpoints',sessionId=sid,operation='remove',stopEpoch=epoch,breakpointId=added['session']['breakpoints'][-1]['breakpointId'])
 else:
  ok('control',sessionId=sid,action='continue',stopEpoch=epoch,waitForStopMs=0)
  time.sleep(.15)
  pause=ok('control',sessionId=sid,action='pause',waitForStopMs=1500)
  epoch=pause['session']['stopEpoch']
  stack=ok('read_stack',sessionId=sid,stopEpoch=epoch,threadId='1')['mixedStack']
  boundary=[f for f in stack['frames'] if f['kind']=='hle-boundary'];host=[f for f in stack['frames'] if f['kind']=='host']
  assert len(boundary)==3 and len({f['invocation'] for f in boundary})==3 and len(host)>3,stack
  assert {f['provenance'] for f in boundary}=={'wait-entry-capture','callback-entry-capture'},boundary
  print('MIXED_BOUNDARIES',len(boundary),'HOST_FRAMES',len(host),flush=True)
 print('REAL_FEX_'+mode.upper()+'_MCP_PASS',flush=True)
finally:
 if sid:
  cleanup=call('stop_session',sessionId=sid)
  assert cleanup['succeeded'] and cleanup['ownershipReleased'],cleanup
 (root/('mixed-mcp-'+mode+'-evidence.json')).write_text(json.dumps(events,indent=2))
 c.close()
