import subprocess,json,queue,threading,os,time
class Client:
 def __init__(self,exe,env=None):
  self.stderr=open('/tmp/'+os.path.basename(exe)+'-fex-stderr.log','w')
  self.p=subprocess.Popen([exe],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.stderr,text=True,bufsize=1,env=env)
  self.q=queue.Queue(); self.i=0
  def read():
   for line in self.p.stdout:
    try: self.q.put(json.loads(line))
    except ValueError: pass
  threading.Thread(target=read,daemon=True).start()
 def send(self,m,p=None,notify=False,timeout=30):
  self.i+=1; obj={'jsonrpc':'2.0','method':m,'params':p or {}}
  if not notify: obj['id']=self.i
  self.p.stdin.write(json.dumps(obj)+'\n'); self.p.stdin.flush()
  if notify: return
  deadline=time.monotonic()+timeout
  while True:
   r=self.q.get(timeout=max(.01,deadline-time.monotonic()))
   if r.get('id')==self.i:
    if 'error' in r: raise RuntimeError(r['error'])
    return r['result']
 def init(self):
  self.send('initialize',{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'fex-validation','version':'1'}})
  self.send('notifications/initialized',notify=True)
  return self.send('tools/list')
 def call(self,name,args):
  r=self.send('tools/call',{'name':name,'arguments':args})
  for item in r.get('content',[]):
   if item.get('type')=='text':
    try: return json.loads(item['text'])
    except ValueError: pass
  return r
 def close(self):
  self.p.stdin.close()
  try: self.p.wait(timeout=4)
  except subprocess.TimeoutExpired: self.p.terminate(); self.p.wait(timeout=4)
  self.stderr.close()
if __name__=='__main__':
 results=[]
 for name in ['spatial-debug-tool','scrcpy','native-debugger','gpu-snapshot','ptracy','renderdoc','litep','ida']:
  c=Client('/Users/bytedance/workspace/doubao_wrapper/'+name)
  try:
   data=c.init(); names=[x['name'] for x in data['tools']]
   results.append({'wrapper':name,'count':len(names),'tools':names})
   print(name,len(names),flush=True)
  finally: c.close()
 open('/tmp/fex-mcp-wrapper-smoke.json','w').write(json.dumps(results,indent=2))
