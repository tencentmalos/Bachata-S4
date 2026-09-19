import json,collections,statistics,pathlib
root=pathlib.Path(__file__).parent
all_rows={}
for filename in ['analysis.json','data-analysis.json','final-analysis.json']:
 r=json.loads((root/filename).read_text());d={k:r[k] for k in ['source_path','sha256','process_id','sdk_revision','events','unmatched','open_scopes','first_present_ms','capture_complete','container_complete','truncated','chunks_skipped']}
 d['reads']=dict(count=len(r['io']),bytes=sum(max(x['Result'],0) for x in r['io']),errors=sum(x['Error']!=0 for x in r['io']))
 for key in ['PrepareNs','SyscallNs']:
  a=sorted(x[key] for x in r['io']);d['reads'][key]=dict(total_ns=sum(a),p50_ns=statistics.median(a),p95_ns=a[int(len(a)*.95)],max_ns=max(a))
 d['vm']={};z=next(x['t'] for x in r['bookmarks'] if x['name'].startswith('Session.Generation.'))
 for name in sorted(set(x['name'] for x in r.get('vm',[]))):
  a=[x for x in r['vm'] if x['name']==name];d['vm'][name]=dict(count=len(a),total_ns=sum(x['elapsed_ns'] for x in a),max_ns=max(x['elapsed_ns'] for x in a),last_s=(max(x['end_ns'] for x in a)-z)/1e9)
 d['long_gaps']=[]
 for name in ['VideoOut.PreparedGuestFlips','VideoOut.PresentedGuestFrames']:
  a=sorted([x for x in r.get('progress_samples',[]) if x['name']==name],key=lambda x:x['t'])
  gaps=sorted([(b['t']-a['t'],a,b) for a,b in zip(a,a[1:]) if b['value']>a['value']],key=lambda x:-x[0])[:3]
  for ns,a,b in gaps:
   io=[x for x in r['io'] if a['t']<=x['t']<=b['t']];vm=[x for x in r.get('vm',[]) if x['begin_ns']<b['t'] and x['end_ns']>a['t']]
   d['long_gaps'].append(dict(name=name,start_s=(a['t']-z)/1e9,end_s=(b['t']-z)/1e9,elapsed_ns=ns,before=a['value'],after=b['value'],io_reads=len(io),io_bytes=sum(max(0,x['Result']) for x in io),io_syscall_ns=sum(x['SyscallNs'] for x in io),vm=vm))
 d['main']=r['main'];all_rows[filename]=d
(root/'analysis-summaries.json').write_text(json.dumps(all_rows,indent=2))
print(json.dumps(all_rows['final-analysis.json'],indent=2))
