"""Call the installed gpu-snapshot MCP server directly (new version) on one snapshot."""
import json
import os
import subprocess
import sys
import threading
import queue

config = json.load(open(os.path.expanduser('~/.claude.json'), encoding='utf-8'))
entry = config['mcpServers']['gpu-snapshot']
env = os.environ.copy()
env.update(entry.get('env', {}))
proc = subprocess.Popen([entry['command'], *entry.get('args', [])], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                        encoding='utf-8', env=env)
messages = queue.Queue()


def reader():
    for line in proc.stdout:
        try:
            messages.put(json.loads(line))
        except json.JSONDecodeError:
            pass


threading.Thread(target=reader, daemon=True).start()


def call(request_id, method, params):
    proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': request_id, 'method': method,
                                 'params': params}) + '\n')
    proc.stdin.flush()
    while True:
        message = messages.get(timeout=600)
        if message.get('id') == request_id:
            return message


call(1, 'initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                       'clientInfo': {'name': 'probe', 'version': '1'}})
proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
proc.stdin.flush()
tools = [tool['name'] for tool in call(2, 'tools/list', {})['result']['tools']]
name = next(tool for tool in tools if 'analyze' in tool and 'snapshot' in tool)
print('tool', name, 'version', entry['command'].split('versions')[1].split(os.sep)[1])
reply = call(3, 'tools/call', {'name': name, 'arguments': {
    'snapshotPath': sys.argv[1], 'databasePath': sys.argv[2], 'reviewLocale': 'zh-CN', 'expertMode': len(sys.argv) > 4 and sys.argv[4] == 'expert'}})
text = ''.join(part.get('text', '') for part in reply['result'].get('content', []))
open(sys.argv[3], 'w', encoding='utf-8').write(text)
result = json.loads(text) if text.startswith('{') else {}
print('status', result.get('status'), (result.get('error') or {}).get('message', ''))
print('contexts', json.dumps(result.get('contextCandidates', [])[:3], ensure_ascii=False)[:1500])
proc.terminate()
