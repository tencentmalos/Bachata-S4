import json,re,sys
from pathlib import Path
base=Path(__file__).resolve().parent
runs=[]
for name in ['a1-off','b1-on','a2-off-cold','b2-on-cold']:
 p=base/name
 if not (p/'summary.json').exists():continue
 obs=[json.loads(l) for l in (p/'observations.jsonl').read_text().splitlines()]
 valid=[d for d in obs if 'snapshot_ns' in d and 'pid' in d]
 running=[d for d in valid if d.get('stage')=='Running' and d.get('driver','').startswith('source=')]
 first,last=running[0],running[-1];pid=first['pid']
 trace=json.loads((p/'trace-summary.json').read_text())
 start,end=int(valid[0]['snapshot_ns'])/1e9,int(valid[-1]['snapshot_ns'])/1e9
 log=(p/'logcat-end.txt').read_text(errors='replace')
 pads=[l for l in log.splitlines() if re.search(r'\s'+pid+r'\s+\d+\s+I\s+TU\s*:',l) and 'SDS page align:' in l]
 (p/'sds-executed.txt').write_text('\n'.join(pads)+'\n')
 dmesg=(p/'dmesg-end.txt').read_text(errors='replace');times=[];hits=[]
 for l in dmesg.splitlines():
  m=re.search(r'^\[\s*(\d+\.\d+)\]',l)
  if not m:continue
  t=float(m[1]);times.append(t)
  if start<=t<=end and re.search(r'CP.*opcode|GPU PAGE FAULT|Fault id|GPU write zram|SMMU.*fault',l,re.I):hits.append(l)
 (p/'current-window-kernel-hits.txt').write_text('\n'.join(hits)+'\n')
 caps=[]
 for f in sorted(p.glob('*.h264.status.json')):
  d=json.loads(f.read_text());caps.append({'file':f.name,'eos':d.get('eos'),'encoded_samples':d.get('encoded_samples'),'first_pts_us':d.get('first_codec_pts_us'),'last_pts_us':d.get('last_codec_pts_us')})
 run={'name':name,'pid':pid,'uuid':first['run_uuid'],'window_start_uptime':start,'window_end_uptime':end,'recorded_seconds':round(end-start,3),'first_running_uptime':int(first['snapshot_ns'])/1e9,'last_running_uptime':int(last['snapshot_ns'])/1e9,'first_flip':int(first['guest_flip'].split()[0]),'last_flip':int(last['guest_flip'].split()[0]),'faultcounts':sorted(set(x['faultcount'] for x in obs)),'faultcount_end':(p/'faultcount-end.txt').read_text().strip(),'sds_log_count':len(pads),'kernel_coverage':[min(times),max(times)] if times else [],'kernel_current_window_hits':hits,'trace_header':trace['header'],'trace_first_last':[trace['first_uptime'],trace['last_uptime']],'reclaim_events':trace['event_counts'].get('kgsl_reclaim_memdesc',0),'gpu_fault_events':trace['event_counts'].get('adreno_gpu_fault',0),'captures':caps}
 runs.append(run)
result={'runs':runs,'total_recorded_seconds':round(sum(r['recorded_seconds'] for r in runs),3),'total_observed_flip_delta':sum(r['last_flip']-r['first_flip'] for r in runs),'limitation':'Independent repeated sessions; scene/input timing not deterministic. Trace rings may overwrite; no-fault runs cannot establish efficacy without reproduction in controls.'}
(base/'ab-summary.json').write_text(json.dumps(result,indent=2))
for r in runs:print(r['name'],r['pid'],r['recorded_seconds'],r['last_flip'],r['faultcounts'],'SDS',r['sds_log_count'],'reclaim',r['reclaim_events'])
