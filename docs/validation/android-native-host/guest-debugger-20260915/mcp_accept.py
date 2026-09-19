import sys,json,os
from pathlib import Path
from mcp_stdio import Client
root=Path(__file__).resolve().parents[4]/'build/white-screen-20260915'
root.mkdir(parents=True,exist_ok=True)
c=Client('/Users/bytedance/workspace/doubao_wrapper/native-debugger'); events=[]; sid=None
try:
 c.init()
 def call(name,**args):
  r=c.call('guest_debug.'+name,args); events.append({'tool':name,'args':args,'result':r})
  print(name,json.dumps(r)[:700],flush=True)
  return r
 started=call('start_session',emulator='shadps4-fex',serial='9c2841a4',port=24681,remotePort=24680,expectedArchitecture='i386:x86-64',byteOrder='little',stopOnAttach=True,dryRun=False,stopLeaseMs=30000)
 assert started['succeeded'],started
 sid=started['session']['sessionId']
 paused=call('control',sessionId=sid,action='pause',waitForStopMs=1000)
 assert paused['succeeded'],paused
 epoch=paused['session']['stopEpoch']
 read=call('read_registers',sessionId=sid,stopEpoch=epoch)
 assert read['succeeded'],read
 regs={r['name']:r for r in read['registers']}; base=int(regs['rip']['numericHex'],16)
 assert 'rsp' in regs and 'rax' in regs
 step=call('control',sessionId=sid,action='step',stopEpoch=epoch,threadId='1',stepMode='locked',waitForStopMs=1000)
 assert step['succeeded'],step
 epoch=step['session']['stopEpoch']
 after=call('read_registers',sessionId=sid,stopEpoch=epoch,registerNames=['rip','rax'])
 assert int(after['registers'][0]['numericHex'],16)==base+3
 bp=call('manage_breakpoints',sessionId=sid,operation='add',stopEpoch=epoch,address=hex(base+6),type='software',length=1)
 assert bp['succeeded'],bp
 hit=call('control',sessionId=sid,action='continue',stopEpoch=epoch,waitForStopMs=1000)
 assert hit['succeeded'],hit
 epoch=hit['session']['stopEpoch']
 at=call('read_registers',sessionId=sid,stopEpoch=epoch,registerNames=['rip','rax'])
 assert int(at['registers'][0]['numericHex'],16)==base+6
 refused=call('control',sessionId=sid,action='step',stopEpoch=epoch,threadId='1',stepMode='locked',waitForStopMs=1000)
 assert refused['errorCode']=='software_breakpoint_resume_unsafe',refused
 removed=call('manage_breakpoints',sessionId=sid,operation='remove',stopEpoch=epoch,breakpointId=bp['session']['breakpoints'][0]['breakpointId'])
 assert removed['succeeded'],removed
 stepped=call('control',sessionId=sid,action='step',stopEpoch=epoch,threadId='1',stepMode='locked',waitForStopMs=1000)
 assert stepped['succeeded'],stepped
 result=call('read_registers',sessionId=sid,stopEpoch=stepped['session']['stopEpoch'],registerNames=['rip'])
 assert int(result['registers'][0]['numericHex'],16)==base+3
 print('REAL_FEX_MCP_PASS',flush=True)
finally:
 if sid:
  cleanup=call('stop_session',sessionId=sid)
  assert cleanup['succeeded'],cleanup
 (root/'debugger-new-mcp-evidence.json').write_text(json.dumps(events,indent=2))
 c.close()
