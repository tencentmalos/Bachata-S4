"""Run each ';;'-separated SQL query against a Perfetto trace with trace_processor_shell.
usage: tpq.py <trace> <sql-file>"""
import subprocess
import sys
import tempfile

TP = (r"C:\Users\fangfang\PicoMcp\litep\versions\litep_mcp-0.1.0-local.20261002.70d50ee-win-x64"
      r"\bin\litep\engine\runtimes\win-x64\trace_processor_shell.exe")
trace, sql_file = sys.argv[1], sys.argv[2]
text = open(sql_file, encoding='utf-8').read()
prelude, *queries = text.split('-- @@')
for q in queries:
    title, body = q.split('\n', 1)
    with tempfile.NamedTemporaryFile('w', suffix='.sql', delete=False, encoding='utf-8') as f:
        f.write(prelude + body)
        path = f.name
    out = subprocess.run([TP, '-q', path, trace], capture_output=True, text=True)
    rows = [ln for ln in out.stdout.splitlines()
            if ln and not ln.startswith(('column ', 'Loading', '[')) ]
    print(f'## {title.strip()}')
    for ln in rows:
        print('  ' + ln)
    if out.returncode:
        print(out.stderr[-2000:])
