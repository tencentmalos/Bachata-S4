import json
from pathlib import Path
import re
import subprocess
import sys
import time

out = Path(__file__).resolve().parent
label = sys.argv[1]
adb = ['adb', '-s', '9c2841a4']
def call(*args):
    return subprocess.check_output(adb + list(args), timeout=20)
hostlog = 'files/host/log/android-host.log'
offset = int(call('shell', 'run-as', 'com.shadps4.android', 'stat', '-c', '%s', hostlog))
started = time.monotonic()
meta = {'start_epoch': time.time(), 'host_log_offset': offset, 'actions': []}
(out / (label + '-launch.txt')).write_bytes(call('shell', 'am', 'start', '-W', '-n',
    'com.shadps4.android/.MainActivity', '-f', '0x24000000', '--ez', 'open_last_game', 'true'))
for second in [0, 30, 60, 90, 115, 120, 123, 126, 129, 150, 180]:
    time.sleep(max(0, started + second - time.monotonic()))
    status = call('shell', 'dumpsys', 'activity', 'service',
                  'com.shadps4.android/.service.FexSessionService', 'debug_status')
    (out / f'{label}-t{second}.txt').write_bytes(status)
    text = status.decode()
    if second in (120, 123, 126, 129):
        if 'stage: Running' not in text or 'generation: 1\n' not in text:
            raise RuntimeError('Refuse input: intended Running generation is absent')
        call('shell', 'input', '-d', '0', 'swipe', '1820', '690', '1820', '690', '200')
        meta['actions'].append({'elapsed': time.monotonic() - started,
                               'input': 'Circle hold 200ms'})
    else:
        (out / f'{label}-t{second}.png').write_bytes(call('exec-out', 'screencap', '-p',
                                                     '-d', '4630946441858561667'))
    print(label, second, ' | '.join(x.strip() for x in text.splitlines()
                                   if any(k in x for k in ('pid:', 'generation:', 'stage:', 'guest_flip:', 'host_present:'))),
          flush=True)
    (out / (label + '-meta.json')).write_text(json.dumps(meta, indent=2) + '\n')
pid = re.search(r'pid: (\d+)', text).group(1)
(out / (label + '-logcat.txt')).write_bytes(call('logcat', '-d', '--pid=' + pid, '-v', 'threadtime'))
(out / (label + '-host.log')).write_bytes(call('shell', 'run-as', 'com.shadps4.android',
                                             'tail', '-c', '+' + str(offset + 1), hostlog))
